/*
 * This file Copyright (C) 2007-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: peer-mgr.c 10894 2010-06-27 01:24:47Z charles $
 */

#include <assert.h>
#include <limits.h> /* INT_MAX */
#include <math.h>
#include <string.h> /* memcpy, memcmp, strstr */
#include <stdlib.h> /* qsort */

#include <event.h>

#include "transmission.h"
#include "announcer.h"
#include "bandwidth.h"
#include "bencode.h"
#include "blocklist.h"
#include "clients.h"
#include "completion.h"
#include "crypto.h"
#include "handshake.h"
#include "inout.h" /* tr_ioTestPiece */
#include "instrumentation.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-msgs.h"
#include "playback.h"
#include "ptrarray.h"
#include "session.h"
#include "stats.h" /* tr_statsAddUploaded, tr_statsAddDownloaded */
#include "torrent.h"
#include "utils.h"
#include "webseed.h"

enum
{
    /* how frequently to cull old atoms */
    ATOM_PERIOD_MSEC = ( 60 * 1000 ),

    /* how frequently to change which peers are choked */
    RECHOKE_PERIOD_MSEC = ( 10 * 1000 ),

    /* how frequently to reallocate bandwidth */
    BANDWIDTH_PERIOD_MSEC = 500,

    /* how frequently to age out old piece request lists */
    REFILL_UPKEEP_PERIOD_MSEC = ( 10 * 1000 ),

    /* how frequently to decide which peers live and die */
    RECONNECT_PERIOD_MSEC = 500,

    /* when many peers are available, keep idle ones this long */
    MIN_UPLOAD_IDLE_SECS = ( 60 ),

    /* when few peers are available, keep idle ones this long */
    MAX_UPLOAD_IDLE_SECS = ( 60 * 5 ),

    /* max number of peers to ask for per second overall.
    * this throttle is to avoid overloading the router */
    MAX_CONNECTIONS_PER_SECOND = 12,

    MAX_CONNECTIONS_PER_PULSE = (int)(MAX_CONNECTIONS_PER_SECOND * (RECONNECT_PERIOD_MSEC/1000.0)),

    /* number of bad pieces a peer is allowed to send before we ban them */
    MAX_BAD_PIECES_PER_PEER = 5,

    /* amount of time to keep a list of request pieces lying around
       before it's considered too old and needs to be rebuilt */
    PIECE_LIST_SHELF_LIFE_SECS = 60,

    /* use for bitwise operations w/peer_atom.myflags */
    MYFLAG_BANNED = 1,

    /* use for bitwise operations w/peer_atom.myflags */
    /* unreachable for now... but not banned.
     * if they try to connect to us it's okay */
    MYFLAG_UNREACHABLE = 2,

    /* the minimum we'll wait before attempting to reconnect to a peer */
    MINIMUM_RECONNECT_INTERVAL_SECS = 5,

    /** how long we'll let requests we've made linger before we cancel them */
    REQUEST_TTL_SECS = 120,

    CANCEL_HISTORY_SEC = 120
};


/**
***
**/

enum
{
    UPLOAD_ONLY_UKNOWN,
    UPLOAD_ONLY_YES,
    UPLOAD_ONLY_NO
};

/**
 * Peer information that should be kept even before we've connected and
 * after we've disconnected.  These are kept in a pool of peer_atoms to decide
 * which ones would make good candidates for connecting to, and to watch out
 * for banned peers.
 *
 * @see tr_peer
 * @see tr_peermsgs
 */
struct peer_atom
{
    uint8_t     from;
    uint8_t     flags;              /* these match the added_f flags */
    uint8_t     myflags;            /* flags that aren't defined in added_f */
    uint8_t     uploadOnly;         /* UPLOAD_ONLY_ */
    int8_t      seedProbability;    /* how likely is this to be a seed... [0..100] or -1 for unknown */
    int8_t      blocklisted;        /* -1 for unknown, TRUE for blocklisted, FALSE for not blocklisted */

    tr_port     port;
    uint16_t    numFails;
    time_t      time;               /* when the peer's connection status last changed */
    time_t      piece_data_time;

    time_t      lastConnectionAttemptAt;
    time_t      lastConnectionAt;

    /* similar to a TTL field, but less rigid --
     * if the swarm is small, the atom will be kept past this date. */
    time_t      shelf_date;
    tr_peer   * peer;               /* will be NULL if not connected */
    tr_address  addr;
};

#ifdef NDEBUG
#define tr_isAtom(a) (TRUE)
#else
static tr_bool
tr_isAtom( const struct peer_atom * atom )
{
    return ( atom != NULL )
        && ( atom->from < TR_PEER_FROM__MAX )
        && ( tr_isAddress( &atom->addr ) );
}
#endif

static const char*
tr_atomAddrStr( const struct peer_atom * atom )
{
    return tr_peerIoAddrStr( &atom->addr, atom->port );
}

struct block_request
{
    tr_block_index_t block;
    tr_peer * peer;
    time_t sentAt;
};

struct weighted_piece
{
    tr_piece_index_t index;
    float salt;
    int16_t requestCount;
    int maxDup;
};

/** @brief Opaque, per-torrent data structure for peer connection information */
typedef struct tr_torrent_peers
{
    tr_ptrArray                outgoingHandshakes; /* tr_handshake */
    tr_ptrArray                pool; /* struct peer_atom */
    tr_ptrArray                peers; /* tr_peer */
    tr_ptrArray                webseeds; /* tr_webseed */

    tr_torrent               * tor;
    tr_peer                  * optimistic; /* the optimistic peer, or NULL if none */
    struct tr_peerMgr        * manager;

    tr_bool                    isRunning;
    tr_bool                    needsCompletenessCheck;

    struct block_request     * requests;
    int                        requestCount;
    int                        requestAlloc;

    struct weighted_piece    * pieces;
    int                        pieceCount;

    /* Sorted lists of pieces from high to low priorities for both
     * the rarest-first and the in-order policies */
    tr_piece_index_t         * piecesRarest;
    int                        pieceCountRarest; 
    tr_piece_index_t         * piecesOrder;
    int                        pieceCountOrder;

    /* How many peers have a given piece */
    size_t                   * pieceReplication;
    /* TRUE if the weighted_piece array is sorted.
     * This is usefull to avoid resorting the list everytime we receive 
     * a bitfield (especially at the begining of a download */
    tr_bool                    arePiecesSorted;

    int                        interestedCount;

    /* An arbitrary metric of how congested the downloads are.
     * Based on how many of requests are cancelled and how many are completed.
     * Lower values indicate less congestion. */
    double                     cancelRate;
}
Torrent;


struct tr_peerMgr
{
    tr_session    * session;
    tr_ptrArray     incomingHandshakes; /* tr_handshake */
    struct event  * bandwidthTimer;
    struct event  * rechokeTimer;
    struct event  * refillUpkeepTimer;
    struct event  * atomTimer;
};

#define tordbg( t, ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, tr_torrentName( t->tor ), __VA_ARGS__ ); \
    } while( 0 )

#define dbgmsg( ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, NULL, __VA_ARGS__ ); \
    } while( 0 )



/*
 *   Replication count manipulation functions ( declarations )
 */





/**
 * Increase the replication count of this piece and sort it if the
 * piece list is already sorted
 */
static void
tr_incrReplicationOfPiece( Torrent * t, const size_t index );

/**
 * Increase the replication count of pieces  present in the bitset
 * If the piece list needs to be resorted, it sets the arePiecesSorted
 * field to FALSE
 */
static void
tr_incrReplicationFromBitset( Torrent * t, const tr_bitset * bitset );

/**
 * Increases the replication count of pieces present in the bitfield
 * Sets the arePiecesSorted field to FALSE
 */
static void
tr_incrReplicationFromBitfield( Torrent * t, const tr_bitfield * bitfield );

/**
 * Increase the replication count of every piece
 */
static void
tr_incrReplication( Torrent * t );

/**
 * Decrease the replication count of pieces present in the bitset
 * If the piece list needs to be resorted, it sets the arePiecesSorted
 * field to FALSE
 */
static void
tr_decrReplicationFromBitset( Torrent * t, const tr_bitset * bitset );

/**
 * Return TRUE if the piece list is sorted, FALSE otherwise
 */
static inline tr_bool
tr_isRarestListSorted( const Torrent * t )
{
    return t->arePiecesSorted;
}

static inline void
tr_setRarestListSorted( Torrent * t, const tr_bool areSorted )
{
    t->arePiecesSorted = areSorted;
}


/** 
 * Weighted pieces  ( declaration )
 */
const tr_torrent * weightTorrent;

static void pieceListRebuild( Torrent * t );
static void pieceListRemovePiece( Torrent * t, tr_piece_index_t piece );
static void pieceListRemoveRequest( Torrent * t, tr_block_index_t block );

static int piecePosInOrder( Torrent * t, tr_piece_index_t index );
static int piecePosInRarest( Torrent * t, tr_piece_index_t index );
static inline struct weighted_piece * pieceListLookup( const Torrent * t, tr_piece_index_t index )
{
    return &t->pieces[index] ;
}

static void removeFromSortedLists( Torrent * t, tr_piece_index_t index );
static void insertInSortedLists( Torrent * t, tr_piece_index_t index );

static void pieceListsSort( Torrent * t );
static void pieceListOrderSort( Torrent * t );
static void pieceListRarestSort( Torrent * t );

static void pieceListResortPiece( Torrent * t, tr_piece_index_t index );
static void pieceListOrderResortPiece( Torrent * t, tr_piece_index_t index );
static void pieceListRarestResortPiece( Torrent * t, tr_piece_index_t index );

/**
 *   Zipf Related functions
 */

/* Update the probability associated with each piece.
 * This parameter is stocked in weighted_piece->salt */
static void
updateZipfProbabilities( Torrent * t );

/* Return an array of pieces that can be parsed in order to compute the next
 * requests to make to a specific peer */
static tr_piece_index_t *
createZipfPiecesArray( const Torrent * t, const tr_peer * peer, int * length );


/**
 * Update the maxDup parameter of the corresponding weighted piece.
 * Return TRUE if maxDup was changed, FALSE otherwise
 *
 * This is used in order to request late subpieces when doing streaming.
 *  DEACTIVATED 
 */
static inline tr_bool updateMaxDuplicatesForPiece( tr_torrent * tor, const tr_piece_index_t index )
{
    struct weighted_piece * p = pieceListLookup( tor->torrentPeers, index );
    tr_bool changed = FALSE;

    /* we don't consider late (sub)pieces yet */
    assert( p->maxDup == 1 );
    return FALSE;

    if( index == tr_cpNextInOrdrerPiece( &tor->completion ) )
    {
        int dup = 1;
        const double inOrderProgress = tr_cpInOrderProgress( &tor->completion );
        const double notInOrderProgress = tr_cpTotalProgress( &tor->completion ) - inOrderProgress;

        if( notInOrderProgress >
              1.5 * (double) TR_RAREST_PERCENTAGE *(1.0 - inOrderProgress) /100.0)
        {
            dup = 2;
        }


        if( p->maxDup != dup )
        {
            p->maxDup = dup;
            changed = TRUE;
        }
    }
    else if( p->maxDup > 1 )
    {
        /* this piece is not the next one anymore */
        p->maxDup = 1;
        changed = TRUE;
    }

    return changed;
}


/**
 * Asserts ( declaration )
 *
 * These functions are useful for sanity checking,
 * but are too expensive even for nightly builds...
 * let's leave it disabled but add an easy hook to compile it back in
 */


//#define EXTENSIVE_ASSERT_DEBUG
#ifdef EXTENSIVE_ASSERT_DEBUG

static void assertWeightedPiecesAreSorted( Torrent * t);
static void assertRarestPiecesAreSorted( Torrent * t );
static void assertOrderPiecesAreSorted( Torrent * t );
static void assertReplicationCountIsExact( Torrent * t );
static void assertWeightedListsAreConsistent( Torrent * t );

#else
#define assertWeightedPiecesAreSorted(t)
#define assertRarestPiecesAreSorted(t)
#define assertOrderPiecesAreSorted(t)
#define assertReplicationCountIsExact(t)
#define assertWeightedListsAreConsistent( x )
#endif



/**
***
**/

static inline void
managerLock( const struct tr_peerMgr * manager )
{
    tr_sessionLock( manager->session );
}

static inline void
managerUnlock( const struct tr_peerMgr * manager )
{
    tr_sessionUnlock( manager->session );
}

static inline void
torrentLock( Torrent * torrent )
{
    managerLock( torrent->manager );
}

static inline void
torrentUnlock( Torrent * torrent )
{
    managerUnlock( torrent->manager );
}

static inline int
torrentIsLocked( const Torrent * t )
{
    return tr_sessionIsLocked( t->manager->session );
}

/**
***
**/

static int
handshakeCompareToAddr( const void * va, const void * vb )
{
    const tr_handshake * a = va;

    return tr_compareAddresses( tr_handshakeGetAddr( a, NULL ), vb );
}

static int
handshakeCompare( const void * a, const void * b )
{
    return handshakeCompareToAddr( a, tr_handshakeGetAddr( b, NULL ) );
}

static inline tr_handshake*
getExistingHandshake( tr_ptrArray * handshakes, const tr_address * addr )
{
    if( tr_ptrArrayEmpty( handshakes ) )
        return NULL;

    return tr_ptrArrayFindSorted( handshakes, addr, handshakeCompareToAddr );
}

static int
comparePeerAtomToAddress( const void * va, const void * vb )
{
    const struct peer_atom * a = va;

    return tr_compareAddresses( &a->addr, vb );
}

static int
compareAtomsByAddress( const void * va, const void * vb )
{
    const struct peer_atom * b = vb;

    assert( tr_isAtom( b ) );

    return comparePeerAtomToAddress( va, &b->addr );
}

/**
***
**/

const tr_address *
tr_peerAddress( const tr_peer * peer )
{
    return &peer->atom->addr;
}

static Torrent*
getExistingTorrent( tr_peerMgr *    manager,
                    const uint8_t * hash )
{
    tr_torrent * tor = tr_torrentFindFromHash( manager->session, hash );

    return tor == NULL ? NULL : tor->torrentPeers;
}

static int
peerCompare( const void * a, const void * b )
{
    return tr_compareAddresses( tr_peerAddress( a ), tr_peerAddress( b ) );
}

static struct peer_atom*
getExistingAtom( const Torrent    * t,
                 const tr_address * addr )
{
    Torrent * tt = (Torrent*)t;
    assert( torrentIsLocked( t ) );
    return tr_ptrArrayFindSorted( &tt->pool, addr, comparePeerAtomToAddress );
}

static tr_bool
peerIsInUse( const Torrent * ct, const struct peer_atom * atom )
{
    Torrent * t = (Torrent*) ct;

    assert( torrentIsLocked ( t ) );

    return ( atom->peer != NULL )
        || getExistingHandshake( &t->outgoingHandshakes, &atom->addr )
        || getExistingHandshake( &t->manager->incomingHandshakes, &atom->addr );
}

static tr_peer*
peerConstructor( struct peer_atom * atom )
{
    tr_peer * peer = tr_new0( tr_peer, 1 );

    tr_bitsetConstructor( &peer->have, 0 );

    /* as long as we know the peer doesn't have any piece */
    tr_bitsetSetHaveNone( &peer->have );

    peer->atom = atom;
    atom->peer = peer;

    peer->blocksSentToClient  = tr_historyNew( CANCEL_HISTORY_SEC, 1 );
    peer->blocksSentToPeer    = tr_historyNew( CANCEL_HISTORY_SEC, 1 );
    peer->cancelsSentToClient = tr_historyNew( CANCEL_HISTORY_SEC, 1 );
    peer->cancelsSentToPeer   = tr_historyNew( CANCEL_HISTORY_SEC, 1 );

    return peer;
}

static tr_peer*
getPeer( Torrent * torrent, struct peer_atom * atom )
{
    tr_peer * peer;

    assert( torrentIsLocked( torrent ) );

    peer = atom->peer;

    if( peer == NULL )
    {
        peer = peerConstructor( atom );
        tr_ptrArrayInsertSorted( &torrent->peers, peer, peerCompare );
    }

    return peer;
}

static void peerDeclinedAllRequests( Torrent *, const tr_peer * );

static void
peerDestructor( Torrent * t, tr_peer * peer )
{
    assert( peer != NULL );

    peerDeclinedAllRequests( t, peer );

    if( peer->msgs != NULL )
    {
        tr_peerMsgsUnsubscribe( peer->msgs, peer->msgsTag );
        tr_peerMsgsFree( peer->msgs );
    }

    tr_peerIoClear( peer->io );
    tr_peerIoUnref( peer->io ); /* balanced by the ref in handshakeDoneCB() */

    tr_historyFree( peer->blocksSentToClient  );
    tr_historyFree( peer->blocksSentToPeer    );
    tr_historyFree( peer->cancelsSentToClient );
    tr_historyFree( peer->cancelsSentToPeer   );

    tr_bitsetDestructor( &peer->have );
    tr_bitfieldFree( peer->blame );
    tr_free( peer->client );
    peer->atom->peer = NULL;

    tr_free( peer );
}

static void
removePeer( Torrent * t, tr_peer * peer )
{
    tr_peer * removed;
    struct peer_atom * atom = peer->atom;

    assert( torrentIsLocked( t ) );
    assert( atom );

    atom->time = tr_time( );

    removed = tr_ptrArrayRemoveSorted( &t->peers, peer, peerCompare );

    tr_decrReplicationFromBitset( t, &peer->have );

    assert( removed == peer );
    peerDestructor( t, removed );
}

static void
removeAllPeers( Torrent * t )
{
    while( !tr_ptrArrayEmpty( &t->peers ) )
        removePeer( t, tr_ptrArrayNth( &t->peers, 0 ) );
}

static void
torrentDestructor( void * vt )
{
    Torrent * t = vt;

    assert( t );
    assert( !t->isRunning );
    assert( torrentIsLocked( t ) );
    assert( tr_ptrArrayEmpty( &t->outgoingHandshakes ) );
    assert( tr_ptrArrayEmpty( &t->peers ) );

    tr_ptrArrayDestruct( &t->webseeds, (PtrArrayForeachFunc)tr_webseedFree );
    tr_ptrArrayDestruct( &t->pool, (PtrArrayForeachFunc)tr_free );
    tr_ptrArrayDestruct( &t->outgoingHandshakes, NULL );
    tr_ptrArrayDestruct( &t->peers, NULL );

    tr_free( t->pieceReplication );
    tr_free( t->requests );
    tr_free( t->pieces );
    tr_free( t->piecesOrder );
    tr_free( t->piecesRarest );
    tr_free( t );
}

static void peerCallbackFunc( void * vpeer, void * vevent, void * vt );

static Torrent*
torrentConstructor( tr_peerMgr * manager,
                    tr_torrent * tor )
{
    int       i;
    Torrent * t;

    t = tr_new0( Torrent, 1 );
    t->manager = manager;
    t->tor = tor;
    t->pool = TR_PTR_ARRAY_INIT;
    t->peers = TR_PTR_ARRAY_INIT;
    t->webseeds = TR_PTR_ARRAY_INIT;
    t->outgoingHandshakes = TR_PTR_ARRAY_INIT;
    t->pieceReplication = tr_new0( size_t, tor->info.pieceCount );

    for( i = 0; i < tor->info.webseedCount; ++i )
    {
        tr_webseed * w =
            tr_webseedNew( tor, tor->info.webseeds[i], peerCallbackFunc, t );
        tr_ptrArrayAppend( &t->webseeds, w );
    }

    return t;
}

tr_peerMgr*
tr_peerMgrNew( tr_session * session )
{
    tr_peerMgr * m = tr_new0( tr_peerMgr, 1 );
    m->session = session;
    m->incomingHandshakes = TR_PTR_ARRAY_INIT;
    return m;
}

static void
deleteTimer( struct event ** t )
{
    if( *t != NULL )
    {
        evtimer_del( *t );
        tr_free( *t );
        *t = NULL;
    }
}

static void
deleteTimers( struct tr_peerMgr * m )
{
    deleteTimer( &m->atomTimer );
    deleteTimer( &m->bandwidthTimer );
    deleteTimer( &m->rechokeTimer );
    deleteTimer( &m->refillUpkeepTimer );
}

void
tr_peerMgrFree( tr_peerMgr * manager )
{
    managerLock( manager );

    deleteTimers( manager );

    /* free the handshakes.  Abort invokes handshakeDoneCB(), which removes
     * the item from manager->handshakes, so this is a little roundabout... */
    while( !tr_ptrArrayEmpty( &manager->incomingHandshakes ) )
        tr_handshakeAbort( tr_ptrArrayNth( &manager->incomingHandshakes, 0 ) );

    tr_ptrArrayDestruct( &manager->incomingHandshakes, NULL );

    managerUnlock( manager );
    tr_free( manager );
}

static int
clientIsDownloadingFrom( const tr_torrent * tor, const tr_peer * peer )
{
    if( !tr_torrentHasMetadata( tor ) )
        return TRUE;

    return peer->clientIsInterested && !peer->clientIsChoked;
}

static int
clientIsUploadingTo( const tr_peer * peer )
{
    return peer->peerIsInterested && !peer->peerIsChoked;
}

/***
****
***/

void
tr_peerMgrOnBlocklistChanged( tr_peerMgr * mgr )
{
    tr_torrent * tor = NULL;
    tr_session * session = mgr->session;

    /* we cache whether or not a peer is blocklisted...
       since the blocklist has changed, erase that cached value */
    while(( tor = tr_torrentNext( session, tor )))
    {
        int i;
        Torrent * t = tor->torrentPeers;
        const int n = tr_ptrArraySize( &t->pool );
        for( i=0; i<n; ++i ) {
            struct peer_atom * atom = tr_ptrArrayNth( &t->pool, i );
            atom->blocklisted = -1;
        }
    }
}

static tr_bool
isAtomBlocklisted( tr_session * session, struct peer_atom * atom )
{
    if( atom->blocklisted < 0 )
        atom->blocklisted = tr_sessionIsAddressBlocked( session, &atom->addr );

    assert( tr_isBool( atom->blocklisted ) );
    return atom->blocklisted;
}


/***
****
***/

static void
atomSetSeedProbability( struct peer_atom * atom, int seedProbability )
{
    assert( atom != NULL );
    assert( -1<=seedProbability && seedProbability<=100 );

    atom->seedProbability = seedProbability;

    if( seedProbability == 100 )
    {
        atom->flags |= ADDED_F_SEED_FLAG;

        /* check if the  peer really has all the pieces. 
         * This could be a weakness of the BT protocol if an attacker/peer 
         * send false PEX messages */
        if( atom->peer != NULL && tr_bitsetPercent( &atom->peer->have ) >= 1.0 )
            tr_bitsetSetHaveAll( &atom->peer->have );
    }
    else if( seedProbability != -1 )
        atom->flags &= ~ADDED_F_SEED_FLAG;
}

static void
atomSetSeed( struct peer_atom * atom )
{
    atomSetSeedProbability( atom, 100 );
}

static inline tr_bool
atomIsSeed( const struct peer_atom * atom )
{
    return atom->seedProbability == 100;
}

tr_bool
tr_peerMgrPeerIsSeed( const tr_torrent  * tor,
                      const tr_address  * addr )
{
    tr_bool isSeed = FALSE;
    const Torrent * t = tor->torrentPeers;
    const struct peer_atom * atom = getExistingAtom( t, addr );

    if( atom )
        isSeed = atomIsSeed( atom );

    return isSeed;
}

double tr_peerMgrPeerProgress( const tr_torrent * tor,
                               const tr_peer * peer )
{
    double progress;
    assert( peer != NULL );
    assert( tr_isTorrent( tor ) );

    if( peer->have.haveAll )
        progress = 1.0;
    else if( peer->have.haveNone )
        progress = 0.0;
    else
        progress = (double) tr_bitfieldCountTrueBits( &peer->have.bitfield ) /
                        (double) tor->info.pieceCount ;

    return progress;
}

/**
***  REQUESTS
***
*** There are two data structures associated with managing block requests:
***
*** 1. Torrent::requests, an array of "struct block_request" which keeps
***    track of which blocks have been requested, and when, and by which peers.
***    This is list is used for (a) cancelling requests that have been pending
***    for too long and (b) avoiding duplicate requests before endgame.
***
*** 2. Torrent::pieces, an array of "struct weighted_piece" which lists the
***    pieces that we want to request.  It's used to decide which blocks to
***    return next when tr_peerMgrGetBlockRequests() is called.
**/

/**
*** struct block_request
**/

static int
compareReqByBlock( const void * va, const void * vb )
{
    const struct block_request * a = va;
    const struct block_request * b = vb;

    /* primary key: block */
    if( a->block < b->block ) return -1;
    if( a->block > b->block ) return 1;

    /* secondary key: peer */
    if( a->peer < b->peer ) return -1;
    if( a->peer > b->peer ) return 1;

    return 0;
}

static void
requestListAdd( Torrent * t, tr_block_index_t block, tr_peer * peer )
{
    struct block_request key;

    /* ensure enough room is available... */
    if( t->requestCount + 1 >= t->requestAlloc )
    {
        const int CHUNK_SIZE = 128;
        t->requestAlloc += CHUNK_SIZE;
        t->requests = tr_renew( struct block_request,
                                t->requests, t->requestAlloc );
    }

    /* populate the record we're inserting */
    key.block = block;
    key.peer = peer;
    key.sentAt = tr_time( );

    /* insert the request to our array... */
    {
        tr_bool exact;
        const int pos = tr_lowerBound( &key, t->requests, t->requestCount,
                                       sizeof( struct block_request ),
                                       compareReqByBlock, &exact );
        assert( !exact );
        memmove( t->requests + pos + 1,
                 t->requests + pos,
                 sizeof( struct block_request ) * ( t->requestCount++ - pos ) );
        t->requests[pos] = key;
    }

    if( peer != NULL )
    {
        ++peer->pendingReqsToPeer;
        assert( peer->pendingReqsToPeer >= 0 );
    }

    /*fprintf( stderr, "added request of block %lu from peer %s... "
                       "there are now %d block\n",
                       (unsigned long)block, tr_atomAddrStr( peer->atom ), t->requestCount );*/
}

static struct block_request *
requestListLookup( Torrent * t, tr_block_index_t block, const tr_peer * peer )
{
    struct block_request key;
    key.block = block;
    key.peer = (tr_peer*) peer;

    return bsearch( &key, t->requests, t->requestCount,
                    sizeof( struct block_request ),
                    compareReqByBlock );
}

/* how many peers are we currently requesting this block from... */
static int
countBlockRequests( Torrent * t, tr_block_index_t block )
{
    tr_bool exact;
    int i, n, pos;
    struct block_request key;

    key.block = block;
    key.peer = NULL;
    pos = tr_lowerBound( &key, t->requests, t->requestCount,
                         sizeof( struct block_request ),
                         compareReqByBlock, &exact );

    assert( !exact ); /* shouldn't have a request with .peer == NULL */

    n = 0;
    for( i=pos; i<t->requestCount; ++i ) {
        if( t->requests[i].block == block )
            ++n;
        else
            break;
    }

    return n;
}

static void
decrementPendingReqCount( const struct block_request * b )
{
    if( b->peer != NULL )
        if( b->peer->pendingReqsToPeer > 0 )
            --b->peer->pendingReqsToPeer;
}

static void
requestListRemove( Torrent * t, tr_block_index_t block, const tr_peer * peer )
{
    const struct block_request * b = requestListLookup( t, block, peer );
    if( b != NULL )
    {
        const int pos = b - t->requests;
        assert( pos < t->requestCount );

        decrementPendingReqCount( b );

        tr_removeElementFromArray( t->requests,
                                   pos,
                                   sizeof( struct block_request ),
                                   t->requestCount-- );

        /*fprintf( stderr, "removing request of block %lu from peer %s... "
                           "there are now %d block requests left\n",
                           (unsigned long)block, tr_atomAddrStr( peer->atom ), t->requestCount );*/
    }
}

static void
cancelRequestsOfBlock( Torrent * t, const tr_block_index_t block )
{
    const struct weighted_piece * p = pieceListLookup( t, tr_torBlockPiece( t->tor, block ) );

    if( p->requestCount > 0 )
    {
        tr_bool exact;
        int pos;
        struct block_request key;

        key.block = block;
        key.peer = NULL;
        pos = tr_lowerBound( &key, t->requests, t->requestCount,
                             sizeof( struct block_request ),
                             compareReqByBlock, &exact );

        while( pos<t->requestCount && t->requests[pos].block == block )
        {
            tr_peerMsgsCancel( t->requests[pos].peer->msgs, block );
            requestListRemove( t, block, t->requests[pos].peer );
            pieceListRemoveRequest( t, block );
            pos++;
        }
    }
}

/**
*** struct weighted_piece
**/

enum
{
    PIECES_UNSORTED,
    PIECES_SORTED_BY_INDEX,
    PIECES_SORTED_BY_WEIGHT
};

static inline int getWeight( const tr_torrent * tor, const struct weighted_piece * p)
{
    const int missing = (int) tor->blockCountInPiece - tr_cpCompleteBlocksInPiece( &tor->completion, p->index );
    const int pending = p->requestCount;
    const int maxDup = p->maxDup;
    const int weight = maxDup * missing > pending ?
        ( maxDup * missing - pending -1 ) % missing :
        (int)(tor->blockCountInPiece + missing);

    return weight;
}

/* we try to create a "weight" s.t. high-priority pieces come before others,
 * and that partially-complete pieces come before empty ones. */
static int
comparePieceByRareness( const void * va, const void * vb )
{
    const tr_torrent * tor = weightTorrent;
    const tr_piece_index_t * inda = (tr_piece_index_t *) va;
    const tr_piece_index_t * indb = (tr_piece_index_t *) vb;

    const struct weighted_piece * a = pieceListLookup( tor->torrentPeers, *inda);
    const struct weighted_piece * b = pieceListLookup( tor->torrentPeers, *indb);
    int ia, ib;

    /* primary key: weight */
    ia = getWeight( tor, a );
    ib = getWeight( tor, b );
    if( ia < ib ) return -1;
    if( ia > ib ) return 1;

    /* secondary key: higher priorities go first */
    ia = tor->info.pieces[a->index].priority;
    ib = tor->info.pieces[b->index].priority;
    if( ia > ib ) return -1;
    if( ia < ib ) return 1;

    /* tertiary key: rarest first */
    if( tor->torrentPeers->pieceReplication[a->index] < tor->torrentPeers->pieceReplication[b->index] ) return -1;
    if( tor->torrentPeers->pieceReplication[a->index] > tor->torrentPeers->pieceReplication[b->index] ) return 1;

    /* quaternary key: order or random */
    if( tor->session->randomDecideLast )
    {
        if( a->salt > b->salt ) return -1;
        if( a->salt < b->salt ) return 1;
    }
    else
    {
        if( * inda < * indb ) return -1;
        if( * inda > * indb ) return 1;
    }

    /* okay, they're equal */
    return 0;
}


static int
comparePieceByOrder( const void * va, const void * vb )
{
    const tr_torrent * tor = weightTorrent;
    const tr_piece_index_t * inda = (tr_piece_index_t *) va;
    const tr_piece_index_t * indb = (tr_piece_index_t *) vb;

    const struct weighted_piece * a = pieceListLookup( tor->torrentPeers, *inda);
    const struct weighted_piece * b = pieceListLookup( tor->torrentPeers, *indb);
    int ia, ib;

    /* primary key: weight */
    ia = getWeight( tor, a );
    ib = getWeight( tor, b );
    if( ia < ib ) return -1;
    if( ia > ib ) return 1;

    /* secondary key: higher priorities go first */
    ia = tor->info.pieces[a->index].priority;
    ib = tor->info.pieces[b->index].priority;
    if( ia > ib ) return -1;
    if( ia < ib ) return 1;

    /* Tertiary key : in order */
    if( * inda < * indb ) return -1;
    if( * inda > * indb ) return 1;

    return 0;
}

static void
pieceListsSort( Torrent * t )
{
    pieceListRarestSort( t );
    pieceListOrderSort( t );
}

static void
pieceListRarestSort( Torrent * t )
{
    weightTorrent = t->tor;
    qsort( t->piecesRarest, t->pieceCountRarest,
           sizeof( tr_piece_index_t ),
           comparePieceByRareness );
    tr_setRarestListSorted( t, TRUE );

    assertRarestPiecesAreSorted( t );
}

static void
pieceListOrderSort( Torrent * t )
{
    weightTorrent = t->tor;
    qsort( t->piecesOrder, t->pieceCountOrder,
           sizeof( tr_piece_index_t ), comparePieceByOrder );

    assertOrderPiecesAreSorted( t );
}

static tr_bool
isInEndgame( Torrent * t )
{
    tr_bool endgame = FALSE;

    /* We are in enggame if all the subpieces have already been requested */
    if( ( t->pieces != NULL ) && ( t->pieceCount > 0 ) && t->pieceCountRarest > 0 )
    {
        const struct weighted_piece * p = pieceListLookup( t, t->piecesRarest[0] );
        const int pending = p->requestCount;
        const int missing = tr_cpMissingBlocksInPiece( &t->tor->completion, p->index );
        endgame = pending >= missing;
    }

    /*if( endgame ) fprintf( stderr, "ENDGAME reached\n" );*/
    return endgame;
}




/**
***
**/

void
tr_peerMgrRebuildRequests( tr_torrent * tor )
{
    assert( tr_isTorrent( tor ) );

    pieceListRebuild( tor->torrentPeers );
}

void
tr_peerMgrGetNextRequests( tr_torrent           * tor,
                           tr_peer              * peer,
                           int                    numwant,
                           tr_block_index_t     * setme,
                           int                  * numgot )
{
    int i;
    int got;
    Torrent * t;
    tr_bool endgame;
    tr_bool finished;
    tr_piece_index_t * pieces;
    int pieceCount;
    const tr_bitset * have = &peer->have;

    /* sanity clause */
    assert( tr_isTorrent( tor ) );
    assert( peer->clientIsInterested );
    assert( !peer->clientIsChoked );
    assert( numwant > 0 );

    /* walk through the pieces and find blocks that should be requested */
    got = 0;
    t = tor->torrentPeers;


    /* prep the pieces list */
    if( t->pieces == NULL )
        pieceListRebuild( t );

    if( !tr_isRarestListSorted( t ) )
        pieceListRarestSort( t );

    assertReplicationCountIsExact( t );
    assertWeightedListsAreConsistent( t );
    assertWeightedPiecesAreSorted( t );


    endgame = isInEndgame( t );


    if( tr_torIsZipf( tor ) ) /* Zipf */
    {
        pieces = createZipfPiecesArray( t, peer, &pieceCount );
    }
    else /* Portion */
    {
        if( tr_cryptoWeakRandInt( 100 ) >= tor->session->rarestPortion )
        {
            /*choose in order */
            pieces = t->piecesOrder;
            pieceCount = t->pieceCountOrder;
        }
        else
        {
            /* choose with rarest-first */
            pieces = t->piecesRarest;
            pieceCount = t->pieceCountRarest;
        }
    }

    i = 0;
    finished = i>=pieceCount || got>=numwant;

    while( !finished )
    {
        struct weighted_piece * p = pieceListLookup( t, pieces[i] );
        const int missing = tr_cpMissingBlocksInPiece( &tor->completion, p->index );
        const int maxDuplicatesPerBlock = endgame ? 3 : p->maxDup;

        /* if the peer has this piece that we want... */
        if( p->requestCount <= ( missing * maxDuplicatesPerBlock ) && tr_bitsetHasFast( have, p->index ) )
        {
            tr_block_index_t b = tr_torPieceFirstBlock( tor, p->index );
            const tr_block_index_t e = b + tr_torPieceCountBlocks( tor, p->index );

            for( ; b!=e && got<numwant; ++b )
            {
                /* don't request blocks we've already got */
                if( tr_cpBlockIsCompleteFast( &tor->completion, b ) )
                    continue;

                /* don't send the same request to the same peer twice */
                if( tr_peerMgrDidPeerRequest( tor, peer, b ) )
                    continue;

                /* don't send the same request to any peer too many times */
                if( countBlockRequests( t, b ) >= maxDuplicatesPerBlock )
                    continue;

                /* update the caller's table */
                setme[got++] = b;

                /* update our own tables */
                requestListAdd( t, b, peer );
                p->requestCount++ ;
            }
        }

        ++i;
        finished = i>=pieceCount || got>+numwant;
    }

    /* In most cases we've just changed the weights of a small number of pieces.
     * So rather than qsort()ing the entire array, it's faster to apply an
     * adaptive insertion sort algorithm. */
    if( got > 0 )
    {
        tr_piece_index_t * tmp;
        int it, nbPiecesRequested;

        weightTorrent = t->tor;

        tmp = (tr_piece_index_t *) tr_new( tr_piece_index_t, i+1 );

        nbPiecesRequested = 0;

        /* save all the modified pieces */
        for( it=0 ; it<got ; it++ )
        {
            if( nbPiecesRequested == 0 
                || tmp[nbPiecesRequested-1] != tr_torBlockPiece( t->tor, setme[it] ) )
            {
                tmp[nbPiecesRequested] = tr_torBlockPiece( t->tor, setme[it] );
                nbPiecesRequested++;
            }
        }


        /* remove each modified piece to have a clean list */
        for( it=0 ; it<nbPiecesRequested ; it++ )
        {
            removeFromSortedLists( t, tmp[it]);
        }

        /* reinsert the pieces removed */
        for( it=0 ; it<nbPiecesRequested ; it++ )
        {
            insertInSortedLists( t, tmp[it] );
        }
        tr_free( tmp );

    }

    if( tr_torIsZipf( tor ) && pieces != NULL )
        tr_free( pieces );

    assertWeightedListsAreConsistent( t );
    assertWeightedPiecesAreSorted( t );
    *numgot = got;
}

    tr_bool
tr_peerMgrDidPeerRequest( const tr_torrent  * tor,
        const tr_peer     * peer,
        tr_block_index_t    block )
{
    const Torrent * t = tor->torrentPeers;
    return requestListLookup( (Torrent*)t, block, peer ) != NULL;
}

/* cancel requests that are too old */
    static void
refillUpkeep( int foo UNUSED, short bar UNUSED, void * vmgr )
{
    time_t now;
    uint64_t now_msec;
    time_t too_old;
    tr_torrent * tor;
    tr_peerMgr * mgr = vmgr;
    managerLock( mgr );

    now = tr_time( );
    now_msec = tr_date( );
    too_old = now - REQUEST_TTL_SECS;

    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor )))
    {
        Torrent * t = tor->torrentPeers;
        const int n = t->requestCount;
        if( n > 0 )
        {
            int keepCount = 0;
            int cancelCount = 0;
            struct block_request * cancel = tr_new( struct block_request, n );
            const struct block_request * it;
            const struct block_request * end;

            for( it=t->requests, end=it+n; it!=end; ++it )
            {
                if( ( it->sentAt <= too_old ) && !tr_peerMsgsIsReadingBlock( it->peer->msgs, it->block ) )
                    cancel[cancelCount++] = *it;
                else
                {
                    if( it != &t->requests[keepCount] )
                        t->requests[keepCount] = *it;
                    keepCount++;
                }
            }

            /* prune out the ones we aren't keeping */
            t->requestCount = keepCount;

            /* send cancel messages for all the "cancel" ones */
            for( it=cancel, end=it+cancelCount; it!=end; ++it ) {
                if( ( it->peer != NULL ) && ( it->peer->msgs != NULL ) ) {
                    tr_historyAdd( it->peer->cancelsSentToPeer, now_msec, 1 );
                    tr_peerMsgsCancel( it->peer->msgs, it->block );
                    decrementPendingReqCount( it );
                }
            }

            /* decrement the pending request counts for the timed-out blocks */
            for( it=cancel, end=it+cancelCount; it!=end; ++it )
                pieceListRemoveRequest( t, it->block );

            /* cleanup loop */
            tr_free( cancel );
        }
    }

    tr_timerAddMsec( mgr->refillUpkeepTimer, REFILL_UPKEEP_PERIOD_MSEC );
    managerUnlock( mgr );
}

static void
addStrike( Torrent * t, tr_peer * peer )
{
    tordbg( t, "increasing peer %s strike count to %d",
            tr_atomAddrStr( peer->atom ), peer->strikes + 1 );

    if( ++peer->strikes >= MAX_BAD_PIECES_PER_PEER )
    {
        struct peer_atom * atom = peer->atom;
        atom->myflags |= MYFLAG_BANNED;
        peer->doPurge = 1;
        tordbg( t, "banning peer %s", tr_atomAddrStr( atom ) );
    }
}

static void
gotBadPiece( Torrent * t, tr_piece_index_t pieceIndex )
{
    tr_torrent *   tor = t->tor;
    const uint32_t byteCount = tr_torPieceCountBytes( tor, pieceIndex );

    tor->corruptCur += byteCount;
    tor->downloadedCur -= MIN( tor->downloadedCur, byteCount );

    tr_announcerAddBytes( tor, TR_ANN_CORRUPT, byteCount );
}

static void
peerSuggestedPiece( Torrent            * t UNUSED,
                    tr_peer            * peer UNUSED,
                    tr_piece_index_t     pieceIndex UNUSED,
                    int                  isFastAllowed UNUSED )
{
#if 0
    assert( t );
    assert( peer );
    assert( peer->msgs );

    /* is this a valid piece? */
    if(  pieceIndex >= t->tor->info.pieceCount )
        return;

    /* don't ask for it if we've already got it */
    if( tr_cpPieceIsComplete( t->tor->completion, pieceIndex ) )
        return;

    /* don't ask for it if they don't have it */
    if( !tr_bitfieldHas( peer->have, pieceIndex ) )
        return;

    /* don't ask for it if we're choked and it's not fast */
    if( !isFastAllowed && peer->clientIsChoked )
        return;

    /* request the blocks that we don't have in this piece */
    {
        tr_block_index_t block;
        const tr_torrent * tor = t->tor;
        const tr_block_index_t start = tr_torPieceFirstBlock( tor, pieceIndex );
        const tr_block_index_t end = start + tr_torPieceCountBlocks( tor, pieceIndex );

        for( block=start; block<end; ++block )
        {
            if( !tr_cpBlockIsComplete( tor->completion, block ) )
            {
                const uint32_t offset = getBlockOffsetInPiece( tor, block );
                const uint32_t length = tr_torBlockCountBytes( tor, block );
                tr_peerMsgsAddRequest( peer->msgs, pieceIndex, offset, length );
                incrementPieceRequests( t, pieceIndex );
            }
        }
    }
#endif
}

static void
removeRequestFromTables( Torrent * t, tr_block_index_t block, const tr_peer * peer )
{
    requestListRemove( t, block, peer );
    pieceListRemoveRequest( t, block );
}

/* peer choked us, or maybe it disconnected.
   either way we need to remove all its requests */
static void
peerDeclinedAllRequests( Torrent * t, const tr_peer * peer )
{
    int i, n;
    tr_block_index_t * blocks = tr_new( tr_block_index_t, t->requestCount );

    for( i=n=0; i<t->requestCount; ++i )
        if( peer == t->requests[i].peer )
            blocks[n++] = t->requests[i].block;

    for( i=0; i<n; ++i )
        removeRequestFromTables( t, blocks[i], peer );

    tr_free( blocks );
}

static void
peerCallbackFunc( void * vpeer, void * vevent, void * vt )
{
    tr_peer * peer = vpeer; /* may be NULL if peer is a webseed */
    Torrent * t = vt;
    const tr_peer_event * e = vevent;

    torrentLock( t );

    switch( e->eventType )
    {
        case TR_PEER_PEER_GOT_DATA:
        {
            const time_t now = tr_time( );
            tr_torrent * tor = t->tor;

            tr_torrentSetActivityDate( tor, now );

            if( e->wasPieceData ) {
                tor->uploadedCur += e->length;
                tr_announcerAddBytes( tor, TR_ANN_UP, e->length );
                tr_torrentSetDirty( tor );
            }

            /* update the stats */
            if( e->wasPieceData )
                tr_statsAddUploaded( tor->session, e->length );

            /* update our atom */
            if( peer && e->wasPieceData )
                peer->atom->piece_data_time = now;

            break;
        }

        case TR_PEER_PEER_GOT_HAVE:
        {
            tr_incrReplicationOfPiece( t, e->pieceIndex );
            break;
        }

        case TR_PEER_PEER_GOT_BITFIELD:
        {
            assert( e->bitset != NULL );
            tr_incrReplicationFromBitset( t, e->bitset );
            break;
        }

        case TR_PEER_CLIENT_GOT_REJ:
            removeRequestFromTables( t, _tr_block( t->tor, e->pieceIndex, e->offset ), peer );
            break;

        case TR_PEER_CLIENT_GOT_CHOKE:
            peerDeclinedAllRequests( t, peer );
            break;

        case TR_PEER_CLIENT_GOT_PORT:
            if( peer )
                peer->atom->port = e->port;
            break;

        case TR_PEER_CLIENT_GOT_SUGGEST:
            if( peer )
                peerSuggestedPiece( t, peer, e->pieceIndex, FALSE );
            break;

        case TR_PEER_CLIENT_GOT_ALLOWED_FAST:
            if( peer )
                peerSuggestedPiece( t, peer, e->pieceIndex, TRUE );
            break;

        case TR_PEER_CLIENT_GOT_DATA:
        {
            const time_t now = tr_time( );
            tr_torrent * tor = t->tor;

            tr_torrentSetActivityDate( tor, now );

            if( e->wasPieceData ) {
                tor->downloadedCur += e->length;
                tr_torrentSetDirty( tor );
            }

            /* update the stats */
            if( e->wasPieceData )
                tr_statsAddDownloaded( tor->session, e->length );

            /* update our atom */
            if( peer && e->wasPieceData )
                peer->atom->piece_data_time = now;

            break;
        }

        case TR_PEER_PEER_PROGRESS:
        {
            if( peer )
            {
                struct peer_atom * atom = peer->atom;
                if( e->progress >= 1.0 ) {
                    tordbg( t, "marking peer %s as a seed", tr_atomAddrStr( atom ) );
                    atomSetSeed( atom );
                }
            }
            break;
        }

        case TR_PEER_CLIENT_GOT_BLOCK:
        {
            tr_torrent * tor = t->tor;
            tr_block_index_t block = _tr_block( tor, e->pieceIndex, e->offset );

            requestListRemove( t, block, peer );
            pieceListRemoveRequest( t, block );
            cancelRequestsOfBlock( t, block );

            tr_instruMsg( t->tor->session, "TR %d R P %s i %u b %u",
                    t->tor->uniqueId,
                    tr_peerIoGetAddrStr( peer->io ),
                    tr_torBlockPiece( t->tor, block ),
                    e->offset );

            if( peer != NULL )
                tr_historyAdd( peer->blocksSentToClient, tr_date( ), 1 );

            if( tr_cpBlockIsComplete( &tor->completion, block ) )
            {
                /* we already have this block... */
                const uint32_t n = tr_torBlockCountBytes( tor, block );
                tor->downloadedCur -= MIN( tor->downloadedCur, n );
                tordbg( t, "we have this block already..." );
            }
            else
            {
                tr_cpBlockAdd( &tor->completion, block );
                updateMaxDuplicatesForPiece( tor, e->pieceIndex );
                pieceListResortPiece( t, e->pieceIndex );

                /* if the torrent has not finished downloading, update the next piece */
                if( tr_cpNextInOrdrerPiece( &tor->completion ) < (tr_piece_index_t) t->pieceCount )
                {
                    if( updateMaxDuplicatesForPiece( tor, tr_cpNextInOrdrerPiece( &tor->completion ) ) )
                        pieceListResortPiece( t, tr_cpNextInOrdrerPiece( &tor->completion ) );
                }

                tr_torrentSetDirty( tor );

                if( tr_cpPieceIsComplete( &tor->completion, e->pieceIndex ) )
                {
                    const tr_piece_index_t p = e->pieceIndex;
                    const tr_bool ok = tr_ioTestPiece( tor, p );

                    if( !ok )
                    {
                        tr_torerr( tor, _( "Piece %lu, which was just downloaded, failed its checksum test" ),
                                   (unsigned long)p );
                    }
                    else 
                    {
                        tr_instruMsg(t->tor->session, "TR %d R PI %lu", t->tor->uniqueId, (unsigned long) p );
                        tr_playbackSetTorrent( tor );
                    }
                    tr_torrentSetHasPiece( tor, p, ok );
                    tr_torrentSetPieceChecked( tor, p, TRUE );
                    tr_peerMgrSetBlame( tor, p, ok );

                    if( tr_torIsZipf( tor ) )
                        updateZipfProbabilities( t );

                    if( !ok )
                    {
                        gotBadPiece( t, p );
                    }
                    else
                    {
                        int i;
                        int peerCount;
                        tr_peer ** peers;
                        tr_file_index_t fileIndex;

                        /* only add this to downloadedCur if we got it from a peer --
                         * webseeds shouldn't count against our ratio.  As one tracker
                         * admin put it, "Those pieces are downloaded directly from the
                         * content distributor, not the peers, it is the tracker's job
                         * to manage the swarms, not the web server and does not fit
                         * into the jurisdiction of the tracker." */
                        if( peer != NULL ) {
                            const uint32_t n = tr_torPieceCountBytes( tor, p );
                            tr_announcerAddBytes( tor, TR_ANN_DOWN, n );
                        }

                        peerCount = tr_ptrArraySize( &t->peers );
                        peers = (tr_peer**) tr_ptrArrayBase( &t->peers );
                        for( i=0; i<peerCount; ++i )
                            tr_peerMsgsHave( peers[i]->msgs, p );

                        for( fileIndex=0; fileIndex<tor->info.fileCount; ++fileIndex ) {
                            const tr_file * file = &tor->info.files[fileIndex];
                            if( ( file->firstPiece <= p ) && ( p <= file->lastPiece ) )
                                if( tr_cpFileIsComplete( &tor->completion, fileIndex ) )
                                    tr_torrentFileCompleted( tor, fileIndex );
                        }

                        pieceListRemovePiece( t, p );
                    }
                }

                t->needsCompletenessCheck = TRUE;
            }
            break;
        }

        case TR_PEER_ERROR:
            if( ( e->err == ERANGE ) || ( e->err == EMSGSIZE ) || ( e->err == ENOTCONN ) )
            {
                /* some protocol error from the peer */
                peer->doPurge = 1;
                tordbg( t, "setting %s doPurge flag because we got an ERANGE, EMSGSIZE, or ENOTCONN error",
                        tr_atomAddrStr( peer->atom ) );
            }
            else
            {
                tordbg( t, "unhandled error: %s", tr_strerror( e->err ) );
            }
            break;

        default:
            assert( 0 );
    }

    torrentUnlock( t );
}

static int
getDefaultShelfLife( uint8_t from )
{
    /* in general, peers obtained from firsthand contact
     * are better than those from secondhand, etc etc */
    switch( from )
    {
        case TR_PEER_FROM_INCOMING : return 60 * 60 * 6;
        case TR_PEER_FROM_LTEP     : return 60 * 60 * 6;
        case TR_PEER_FROM_TRACKER  : return 60 * 60 * 3;
        case TR_PEER_FROM_DHT      : return 60 * 60 * 3;
        case TR_PEER_FROM_PEX      : return 60 * 60 * 2;
        case TR_PEER_FROM_RESUME   : return 60 * 60;
        case TR_PEER_FROM_LPD      : return 10 * 60;
        default                    : return 60 * 60;
    }
}

static void
ensureAtomExists( Torrent           * t,
                  const tr_address  * addr,
                  const tr_port       port,
                  const uint8_t       flags,
                  const int8_t        seedProbability,
                  const uint8_t       from )
{
    struct peer_atom * a;

    assert( tr_isAddress( addr ) );
    assert( from < TR_PEER_FROM__MAX );

    a = getExistingAtom( t, addr );

    if( a == NULL )
    {
        const int jitter = tr_cryptoWeakRandInt( 60*10 );
        a = tr_new0( struct peer_atom, 1 );
        a->addr = *addr;
        a->port = port;
        a->flags = flags;
        a->from = from;
        a->shelf_date = tr_time( ) + getDefaultShelfLife( from ) + jitter;
        a->blocklisted = -1;
        atomSetSeedProbability( a, seedProbability );
        tr_ptrArrayInsertSorted( &t->pool, a, compareAtomsByAddress );

        tordbg( t, "got a new atom: %s", tr_atomAddrStr( a ) );
    }
    else if( a->seedProbability == -1 )
    {
        atomSetSeedProbability( a, seedProbability );
    }
}

static int
getMaxPeerCount( const tr_torrent * tor )
{
    return tor->maxConnectedPeers;
}

static int
getPeerCount( const Torrent * t )
{
    return tr_ptrArraySize( &t->peers );/* + tr_ptrArraySize( &t->outgoingHandshakes ); */
}

/* FIXME: this is kind of a mess. */
static tr_bool
myHandshakeDoneCB( tr_handshake  * handshake,
                   tr_peerIo     * io,
                   tr_bool         readAnythingFromPeer,
                   tr_bool         isConnected,
                   const uint8_t * peer_id,
                   void          * vmanager )
{
    tr_bool            ok = isConnected;
    tr_bool            success = FALSE;
    tr_port            port;
    const tr_address * addr;
    tr_peerMgr       * manager = vmanager;
    Torrent          * t;
    tr_handshake     * ours;

    assert( io );
    assert( tr_isBool( ok ) );

    t = tr_peerIoHasTorrentHash( io )
        ? getExistingTorrent( manager, tr_peerIoGetTorrentHash( io ) )
        : NULL;

    if( tr_peerIoIsIncoming ( io ) )
        ours = tr_ptrArrayRemoveSorted( &manager->incomingHandshakes,
                                        handshake, handshakeCompare );
    else if( t )
        ours = tr_ptrArrayRemoveSorted( &t->outgoingHandshakes,
                                        handshake, handshakeCompare );
    else
        ours = handshake;

    assert( ours );
    assert( ours == handshake );

    if( t )
        torrentLock( t );

    addr = tr_peerIoGetAddress( io, &port );

    if( !ok || !t || !t->isRunning )
    {
        if( t )
        {
            struct peer_atom * atom = getExistingAtom( t, addr );
            if( atom )
            {
                ++atom->numFails;

                if( !readAnythingFromPeer )
                {
                    tordbg( t, "marking peer %s as unreachable... numFails is %d", tr_atomAddrStr( atom ), (int)atom->numFails );
                    atom->myflags |= MYFLAG_UNREACHABLE;
                }
            }
        }
    }
    else /* looking good */
    {
        struct peer_atom * atom;

        ensureAtomExists( t, addr, port, 0, -1, TR_PEER_FROM_INCOMING );
        atom = getExistingAtom( t, addr );
        atom->time = tr_time( );
        atom->piece_data_time = 0;
        atom->lastConnectionAt = tr_time( );
        atom->myflags &= ~MYFLAG_UNREACHABLE;

        if( atom->myflags & MYFLAG_BANNED )
        {
            tordbg( t, "banned peer %s tried to reconnect",
                    tr_atomAddrStr( atom ) );
        }
        else if( tr_peerIoIsIncoming( io )
               && ( getPeerCount( t ) >= getMaxPeerCount( t->tor ) ) )

        {
        }
        else
        {
            tr_peer * peer = atom->peer;

            if( peer ) /* we already have this peer */
            {
            }
            else
            {
                peer = getPeer( t, atom );
                tr_free( peer->client );

                if( !peer_id )
                    peer->client = NULL;
                else {
                    char client[128];
                    tr_clientForId( client, sizeof( client ), peer_id );
                    peer->client = tr_strdup( client );
                }

                peer->io = tr_handshakeStealIO( handshake ); /* this steals its refcount too, which is
                                                                balanced by our unref in peerDestructor()  */
                tr_peerIoSetParent( peer->io, t->tor->bandwidth );
                tr_peerMsgsNew( t->tor, peer, peerCallbackFunc, t, &peer->msgsTag );

                {
                    char orig[20];
                    switch(atom->from)
                    {
                        case TR_PEER_FROM_DHT:
                            strcpy( orig, "DHT" );
                            break;
                        case TR_PEER_FROM_PEX:
                            strcpy( orig, "PEX" );
                            break;
                        case TR_PEER_FROM_LPD:
                            strcpy( orig, "LPD" );
                            break;
                        case TR_PEER_FROM_INCOMING:
                            strcpy( orig, "INC" );
                            break;
                        case TR_PEER_FROM_TRACKER:
                            strcpy( orig, "TRACKER" );
                            break;
                        case TR_PEER_FROM_RESUME:
                            strcpy( orig, "RESUME" );
                            break;
                        case TR_PEER_FROM_LTEP:
                            strcpy( orig, "LTEP" );
                            break;
                        case TR_PEER_FROM_CMD:
                            strcpy( orig, "CMD" );
                            break;
                        default:
                            strcpy( orig, "UNKNOWN" );
                    }
                    tr_instruMsg( t->tor->session, "TR %d CONH %s %s ORIG %s",
                                  t->tor->uniqueId,
                                  tr_peerIoIsIncoming( io ) ? "R" : "L",
                                  tr_peerIoGetAddrStr( io ),
                                  orig );
                }
                success = TRUE;
            }
        }
    }

    if( t )
        torrentUnlock( t );

    return success;
}

void
tr_peerMgrAddIncoming( tr_peerMgr * manager,
                       tr_address * addr,
                       tr_port      port,
                       int          socket )
{
    tr_session * session;

    managerLock( manager );

    assert( tr_isSession( manager->session ) );
    session = manager->session;

    if( tr_sessionIsAddressBlocked( session, addr ) )
    {
        tr_dbg( "Banned IP address \"%s\" tried to connect to us", tr_ntop_non_ts( addr ) );
        tr_netClose( session, socket );
    }
    else if( getExistingHandshake( &manager->incomingHandshakes, addr ) )
    {
        tr_netClose( session, socket );
    }
    else /* we don't have a connection to them yet... */
    {
        tr_peerIo *    io;
        tr_handshake * handshake;

        io = tr_peerIoNewIncoming( session, session->bandwidth, addr, port, socket );

        handshake = tr_handshakeNew( io,
                                     session->encryptionMode,
                                     myHandshakeDoneCB,
                                     manager );

        tr_peerIoUnref( io ); /* balanced by the implicit ref in tr_peerIoNewIncoming() */

        tr_ptrArrayInsertSorted( &manager->incomingHandshakes, handshake,
                                 handshakeCompare );
    }

    managerUnlock( manager );
}

static tr_bool
tr_isPex( const tr_pex * pex )
{
    return pex && tr_isAddress( &pex->addr );
}

void
tr_peerMgrAddPex( tr_torrent * tor, uint8_t from,
                  const tr_pex * pex, int8_t seedProbability )
{
    if( tr_isPex( pex ) ) /* safeguard against corrupt data */
    {
        Torrent * t = tor->torrentPeers;
        managerLock( t->manager );

        if( !tr_sessionIsAddressBlocked( t->manager->session, &pex->addr ) )
            if( tr_isValidPeerAddress( &pex->addr, pex->port ) )
            {
                ensureAtomExists( t, &pex->addr, pex->port, pex->flags, seedProbability, from );
            }

        managerUnlock( t->manager );
    }
}

void
tr_peerMgrMarkAllAsSeeds( tr_torrent * tor )
{
    Torrent * t = tor->torrentPeers;
    const int n = tr_ptrArraySize( &t->pool );
    struct peer_atom ** it = (struct peer_atom**) tr_ptrArrayBase( &t->pool );
    struct peer_atom ** end = it + n;

    while( it != end )
        atomSetSeed( *it++ );
}

tr_pex *
tr_peerMgrCompactToPex( const void *    compact,
                        size_t          compactLen,
                        const uint8_t * added_f,
                        size_t          added_f_len,
                        size_t *        pexCount )
{
    size_t          i;
    size_t          n = compactLen / 6;
    const uint8_t * walk = compact;
    tr_pex *        pex = tr_new0( tr_pex, n );

    for( i = 0; i < n; ++i )
    {
        pex[i].addr.type = TR_AF_INET;
        memcpy( &pex[i].addr.addr, walk, 4 ); walk += 4;
        memcpy( &pex[i].port, walk, 2 ); walk += 2;
        if( added_f && ( n == added_f_len ) )
            pex[i].flags = added_f[i];
    }

    *pexCount = n;
    return pex;
}

tr_pex *
tr_peerMgrCompact6ToPex( const void    * compact,
                         size_t          compactLen,
                         const uint8_t * added_f,
                         size_t          added_f_len,
                         size_t        * pexCount )
{
    size_t          i;
    size_t          n = compactLen / 18;
    const uint8_t * walk = compact;
    tr_pex *        pex = tr_new0( tr_pex, n );

    for( i = 0; i < n; ++i )
    {
        pex[i].addr.type = TR_AF_INET6;
        memcpy( &pex[i].addr.addr.addr6.s6_addr, walk, 16 ); walk += 16;
        memcpy( &pex[i].port, walk, 2 ); walk += 2;
        if( added_f && ( n == added_f_len ) )
            pex[i].flags = added_f[i];
    }

    *pexCount = n;
    return pex;
}

tr_pex *
tr_peerMgrArrayToPex( const void * array,
                      size_t       arrayLen,
                      size_t      * pexCount )
{
    size_t          i;
    size_t          n = arrayLen / ( sizeof( tr_address ) + 2 );
    /*size_t          n = arrayLen / sizeof( tr_peerArrayElement );*/
    const uint8_t * walk = array;
    tr_pex        * pex = tr_new0( tr_pex, n );

    for( i = 0 ; i < n ; i++ ) {
        memcpy( &pex[i].addr, walk, sizeof( tr_address ) );
        memcpy( &pex[i].port, walk + sizeof( tr_address ), 2 );
        pex[i].flags = 0x00;
        walk += sizeof( tr_address ) + 2;
    }

    *pexCount = n;
    return pex;
}

/**
***
**/

void
tr_peerMgrSetBlame( tr_torrent     * tor,
                    tr_piece_index_t pieceIndex,
                    int              success )
{
    if( !success )
    {
        int        peerCount, i;
        Torrent *  t = tor->torrentPeers;
        tr_peer ** peers;

        assert( torrentIsLocked( t ) );

        peers = (tr_peer **) tr_ptrArrayPeek( &t->peers, &peerCount );
        for( i = 0; i < peerCount; ++i )
        {
            tr_peer * peer = peers[i];
            if( tr_bitfieldHas( peer->blame, pieceIndex ) )
            {
                tordbg( t, "peer %s contributed to corrupt piece (%d); now has %d strikes",
                        tr_atomAddrStr( peer->atom ),
                        pieceIndex, (int)peer->strikes + 1 );
                addStrike( t, peer );
            }
        }
    }
}

int
tr_pexCompare( const void * va, const void * vb )
{
    const tr_pex * a = va;
    const tr_pex * b = vb;
    int i;

    assert( tr_isPex( a ) );
    assert( tr_isPex( b ) );

    if(( i = tr_compareAddresses( &a->addr, &b->addr )))
        return i;

    if( a->port != b->port )
        return a->port < b->port ? -1 : 1;

    return 0;
}

#if 0
static int
peerPrefersCrypto( const tr_peer * peer )
{
    if( peer->encryption_preference == ENCRYPTION_PREFERENCE_YES )
        return TRUE;

    if( peer->encryption_preference == ENCRYPTION_PREFERENCE_NO )
        return FALSE;

    return tr_peerIoIsEncrypted( peer->io );
}
#endif

/* better goes first */
static int
compareAtomsByUsefulness( const void * va, const void *vb )
{
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;

    assert( tr_isAtom( a ) );
    assert( tr_isAtom( b ) );

    if( a->piece_data_time != b->piece_data_time )
        return a->piece_data_time > b->piece_data_time ? -1 : 1;
    if( a->from != b->from )
        return a->from < b->from ? -1 : 1;
    if( a->numFails != b->numFails )
        return a->numFails < b->numFails ? -1 : 1;

    return 0;
}

int
tr_peerMgrGetPeers( tr_torrent   * tor,
                    tr_pex      ** setme_pex,
                    uint8_t        af,
                    uint8_t        list_mode,
                    int            maxCount )
{
    int i;
    int n;
    int count = 0;
    int atomCount = 0;
    const Torrent * t = tor->torrentPeers;
    struct peer_atom ** atoms = NULL;
    tr_pex * pex;
    tr_pex * walk;

    assert( tr_isTorrent( tor ) );
    assert( setme_pex != NULL );
    assert( af==TR_AF_INET || af==TR_AF_INET6 );
    assert( list_mode==TR_PEERS_CONNECTED || list_mode==TR_PEERS_ALL );

    managerLock( t->manager );

    /**
    ***  build a list of atoms
    **/

    if( list_mode == TR_PEERS_CONNECTED ) /* connected peers only */
    {
        int i;
        const tr_peer ** peers = (const tr_peer **) tr_ptrArrayBase( &t->peers );
        atomCount = tr_ptrArraySize( &t->peers );
        atoms = tr_new( struct peer_atom *, atomCount );
        for( i=0; i<atomCount; ++i )
            atoms[i] = peers[i]->atom;
    }
    else /* TR_PEERS_ALL */
    {
        const struct peer_atom ** atomsBase = (const struct peer_atom**) tr_ptrArrayBase( &t->pool );
        atomCount = tr_ptrArraySize( &t->pool );
        atoms = tr_memdup( atomsBase, atomCount * sizeof( struct peer_atom * ) );
    }

    qsort( atoms, atomCount, sizeof( struct peer_atom * ), compareAtomsByUsefulness );

    /**
    ***  add the first N of them into our return list
    **/

    n = MIN( atomCount, maxCount );
    pex = walk = tr_new0( tr_pex, n );

    for( i=0; i<atomCount && count<n; ++i )
    {
        const struct peer_atom * atom = atoms[i];
        if( atom->addr.type == af )
        {
            assert( tr_isAddress( &atom->addr ) );
            walk->addr = atom->addr;
            walk->port = atom->port;
            walk->flags = atom->flags;
            ++count;
            ++walk;
        }
    }

    qsort( pex, count, sizeof( tr_pex ), tr_pexCompare );

    assert( ( walk - pex ) == count );
    *setme_pex = pex;

    /* cleanup */
    tr_free( atoms );
    managerUnlock( t->manager );
    return count;
}

static void atomPulse      ( int, short, void * );
static void bandwidthPulse ( int, short, void * );
static void rechokePulse   ( int, short, void * );
static void reconnectPulse ( int, short, void * );

static struct event *
createTimer( int msec, void (*callback)(int, short, void *), void * cbdata )
{
    struct event * timer = tr_new0( struct event, 1 );
    evtimer_set( timer, callback, cbdata );
    tr_timerAddMsec( timer, msec );
    return timer;
}

static void
ensureMgrTimersExist( struct tr_peerMgr * m )
{
    if( m->atomTimer == NULL )
        m->atomTimer = createTimer( ATOM_PERIOD_MSEC, atomPulse, m );

    if( m->bandwidthTimer == NULL )
        m->bandwidthTimer = createTimer( BANDWIDTH_PERIOD_MSEC, bandwidthPulse, m );

    if( m->rechokeTimer == NULL )
        m->rechokeTimer = createTimer( RECHOKE_PERIOD_MSEC, rechokePulse, m );

   if( m->refillUpkeepTimer == NULL )
        m->refillUpkeepTimer = createTimer( REFILL_UPKEEP_PERIOD_MSEC, refillUpkeep, m );
}

void
tr_peerMgrStartTorrent( tr_torrent * tor )
{
    Torrent * t = tor->torrentPeers;

    assert( t != NULL );
    managerLock( t->manager );
    ensureMgrTimersExist( t->manager );

    memset( t->pieceReplication, 0, sizeof( size_t ) * t->tor->info.pieceCount );
    tr_setRarestListSorted( t, FALSE );

    t->isRunning = TRUE;

    rechokePulse( 0, 0, t->manager );
    managerUnlock( t->manager );
}

static void
stopTorrent( Torrent * t )
{
    int i, n;

    assert( torrentIsLocked( t ) );

    t->isRunning = FALSE;

    memset( t->pieceReplication, 0, sizeof( size_t ) * t->tor->info.pieceCount );
    tr_setRarestListSorted( t, FALSE );

    /* disconnect the peers. */
    for( i=0, n=tr_ptrArraySize( &t->peers ); i<n; ++i )
        peerDestructor( t, tr_ptrArrayNth( &t->peers, i ) );
    tr_ptrArrayClear( &t->peers );

    /* disconnect the handshakes.  handshakeAbort calls handshakeDoneCB(),
     * which removes the handshake from t->outgoingHandshakes... */
    while( !tr_ptrArrayEmpty( &t->outgoingHandshakes ) )
        tr_handshakeAbort( tr_ptrArrayNth( &t->outgoingHandshakes, 0 ) );
}

void
tr_peerMgrStopTorrent( tr_torrent * tor )
{
    Torrent * t = tor->torrentPeers;

    managerLock( t->manager );

    stopTorrent( t );

    managerUnlock( t->manager );
}

void
tr_peerMgrAddTorrent( tr_peerMgr * manager,
                      tr_torrent * tor )
{
    managerLock( manager );

    assert( tor );
    assert( tor->torrentPeers == NULL );

    tor->torrentPeers = torrentConstructor( manager, tor );

    managerUnlock( manager );
}

void
tr_peerMgrRemoveTorrent( tr_torrent * tor )
{
    tr_torrentLock( tor );

    stopTorrent( tor->torrentPeers );
    torrentDestructor( tor->torrentPeers );

    tr_torrentUnlock( tor );
}

void
tr_peerMgrTorrentAvailability( const tr_torrent * tor,
                               int8_t           * tab,
                               unsigned int       tabCount )
{
    tr_piece_index_t   i;
    const Torrent *    t;
    float              interval;
    tr_bool            isSeed;
    int                peerCount;
    const tr_peer **   peers;
    tr_torrentLock( tor );

    t = tor->torrentPeers;
    tor = t->tor;
    interval = tor->info.pieceCount / (float)tabCount;
    isSeed = tor && ( tr_cpGetStatus ( &tor->completion ) == TR_SEED );
    peers = (const tr_peer **) tr_ptrArrayBase( &t->peers );
    peerCount = tr_ptrArraySize( &t->peers );

    memset( tab, 0, tabCount );

    for( i = 0; tor && i < tabCount; ++i )
    {
        const int piece = i * interval;

        if( isSeed || tr_cpPieceIsComplete( &tor->completion, piece ) )
            tab[i] = -1;
        else if( peerCount ) {
            int j;
            for( j = 0; j < peerCount; ++j )
                if( tr_bitsetHas( &peers[j]->have, i ) )
                    ++tab[i];
        }
    }

    tr_torrentUnlock( tor );
}

/* Returns the pieces that are available from peers */
tr_bitfield*
tr_peerMgrGetAvailable( const tr_torrent * tor )
{
    int i;
    int peerCount;
    Torrent * t = tor->torrentPeers;
    const tr_peer ** peers;
    tr_bitfield * pieces;
    managerLock( t->manager );

    pieces = tr_bitfieldNew( t->tor->info.pieceCount );
    peerCount = tr_ptrArraySize( &t->peers );
    peers = (const tr_peer**) tr_ptrArrayBase( &t->peers );
    for( i=0; i<peerCount; ++i )
        tr_bitsetOr( pieces, &peers[i]->have );

    managerUnlock( t->manager );
    return pieces;
}

void
tr_peerMgrTorrentStats( tr_torrent       * tor,
                        int              * setmePeersKnown,
                        int              * setmePeersConnected,
                        int              * setmeSeedsConnected,
                        int              * setmeWebseedsSendingToUs,
                        int              * setmePeersSendingToUs,
                        int              * setmePeersGettingFromUs,
                        int              * setmePeersFrom )
{
    int i, size;
    const Torrent * t = tor->torrentPeers;
    const tr_peer ** peers;
    const tr_webseed ** webseeds;

    managerLock( t->manager );

    peers = (const tr_peer **) tr_ptrArrayBase( &t->peers );
    size = tr_ptrArraySize( &t->peers );

    *setmePeersKnown           = tr_ptrArraySize( &t->pool );
    *setmePeersConnected       = 0;
    *setmeSeedsConnected       = 0;
    *setmePeersGettingFromUs   = 0;
    *setmePeersSendingToUs     = 0;
    *setmeWebseedsSendingToUs  = 0;

    for( i=0; i<TR_PEER_FROM__MAX; ++i )
        setmePeersFrom[i] = 0;

    for( i=0; i<size; ++i )
    {
        const tr_peer * peer = peers[i];
        const struct peer_atom * atom = peer->atom;

        if( peer->io == NULL ) /* not connected */
            continue;

        ++*setmePeersConnected;

        ++setmePeersFrom[atom->from];

        if( clientIsDownloadingFrom( tor, peer ) )
            ++*setmePeersSendingToUs;

        if( clientIsUploadingTo( peer ) )
            ++*setmePeersGettingFromUs;

        if( atomIsSeed( atom ) )
            ++*setmeSeedsConnected;
    }

    webseeds = (const tr_webseed**) tr_ptrArrayBase( &t->webseeds );
    size = tr_ptrArraySize( &t->webseeds );
    for( i=0; i<size; ++i )
        if( tr_webseedIsActive( webseeds[i] ) )
            ++*setmeWebseedsSendingToUs;

    managerUnlock( t->manager );
}

float
tr_peerMgrGetWebseedSpeed( const tr_torrent * tor, uint64_t now )
{
    int i;
    float tmp;
    float ret = 0;

    const Torrent * t = tor->torrentPeers;
    const int n = tr_ptrArraySize( &t->webseeds );
    const tr_webseed ** webseeds = (const tr_webseed**) tr_ptrArrayBase( &t->webseeds );

    for( i=0; i<n; ++i )
        if( tr_webseedGetSpeed( webseeds[i], now, &tmp ) )
            ret += tmp;

    return ret;
}


float*
tr_peerMgrWebSpeeds( const tr_torrent * tor )
{
    const Torrent * t = tor->torrentPeers;
    const tr_webseed ** webseeds;
    int i;
    int webseedCount;
    float * ret;
    uint64_t now;

    assert( t->manager );
    managerLock( t->manager );

    webseeds = (const tr_webseed**) tr_ptrArrayBase( &t->webseeds );
    webseedCount = tr_ptrArraySize( &t->webseeds );
    assert( webseedCount == tor->info.webseedCount );
    ret = tr_new0( float, webseedCount );
    now = tr_date( );

    for( i=0; i<webseedCount; ++i )
        if( !tr_webseedGetSpeed( webseeds[i], now, &ret[i] ) )
            ret[i] = -1.0;

    managerUnlock( t->manager );
    return ret;
}

double
tr_peerGetPieceSpeed( const tr_peer * peer, uint64_t now, tr_direction direction )
{
    return peer->io ? tr_peerIoGetPieceSpeed( peer->io, now, direction ) : 0.0;
}


struct tr_peer_stat *
tr_peerMgrPeerStats( const tr_torrent    * tor,
                     int                 * setmeCount )
{
    int i, size;
    const Torrent * t = tor->torrentPeers;
    const tr_peer ** peers;
    tr_peer_stat * ret;
    uint64_t now;

    assert( t->manager );
    managerLock( t->manager );

    size = tr_ptrArraySize( &t->peers );
    peers = (const tr_peer**) tr_ptrArrayBase( &t->peers );
    ret = tr_new0( tr_peer_stat, size );
    now = tr_date( );

    for( i=0; i<size; ++i )
    {
        char *                   pch;
        const tr_peer *          peer = peers[i];
        const struct peer_atom * atom = peer->atom;
        tr_peer_stat *           stat = ret + i;

        tr_ntop( &atom->addr, stat->addr, sizeof( stat->addr ) );
        tr_strlcpy( stat->client, ( peer->client ? peer->client : "" ),
                   sizeof( stat->client ) );
        stat->port                = ntohs( peer->atom->port );
        stat->from                = atom->from;
        stat->progress            = peer->progress;
        stat->isEncrypted         = tr_peerIoIsEncrypted( peer->io ) ? 1 : 0;
        stat->rateToPeer          = tr_peerGetPieceSpeed( peer, now, TR_CLIENT_TO_PEER );
        stat->rateToClient        = tr_peerGetPieceSpeed( peer, now, TR_PEER_TO_CLIENT );
        stat->peerIsChoked        = peer->peerIsChoked;
        stat->peerIsInterested    = peer->peerIsInterested;
        stat->clientIsChoked      = peer->clientIsChoked;
        stat->clientIsInterested  = peer->clientIsInterested;
        stat->isIncoming          = tr_peerIoIsIncoming( peer->io );
        stat->isDownloadingFrom   = clientIsDownloadingFrom( tor, peer );
        stat->isUploadingTo       = clientIsUploadingTo( peer );
        stat->isSeed              = ( atom->uploadOnly == UPLOAD_ONLY_YES ) || ( peer->progress >= 1.0 );

        stat->blocksToPeer        = tr_historyGet( peer->blocksSentToPeer,    now, CANCEL_HISTORY_SEC*1000 );
        stat->blocksToClient      = tr_historyGet( peer->blocksSentToClient,  now, CANCEL_HISTORY_SEC*1000 );
        stat->cancelsToPeer       = tr_historyGet( peer->cancelsSentToPeer,   now, CANCEL_HISTORY_SEC*1000 );
        stat->cancelsToClient     = tr_historyGet( peer->cancelsSentToClient, now, CANCEL_HISTORY_SEC*1000 );

        stat->pendingReqsToPeer   = peer->pendingReqsToPeer;
        stat->pendingReqsToClient = peer->pendingReqsToClient;

        pch = stat->flagStr;
        if( t->optimistic == peer ) *pch++ = 'O';
        if( stat->isDownloadingFrom ) *pch++ = 'D';
        else if( stat->clientIsInterested ) *pch++ = 'd';
        if( stat->isUploadingTo ) *pch++ = 'U';
        else if( stat->peerIsInterested ) *pch++ = 'u';
        if( !stat->clientIsChoked && !stat->clientIsInterested ) *pch++ = 'K';
        if( !stat->peerIsChoked && !stat->peerIsInterested ) *pch++ = '?';
        if( stat->isEncrypted ) *pch++ = 'E';
        if( stat->from == TR_PEER_FROM_DHT ) *pch++ = 'H';
        if( stat->from == TR_PEER_FROM_PEX ) *pch++ = 'X';
        if( stat->isIncoming ) *pch++ = 'I';
        *pch = '\0';
    }

    *setmeCount = size;

    managerUnlock( t->manager );
    return ret;
}

/**
***
**/

/* do we still want this piece and does the peer have it? */
static tr_bool
isPieceInteresting( const tr_torrent * tor, const tr_peer * peer, tr_piece_index_t index )
{
    return ( !tor->info.pieces[index].dnd ) /* we want it */
        && ( !tr_cpPieceIsComplete( &tor->completion, index ) )  /* we don't have it */
        && ( tr_bitsetHas( &peer->have, index ) ); /* peer has it */
}

/* does this peer have any pieces that we want? */
static tr_bool
isPeerInteresting( const tr_torrent * tor, const tr_peer * peer )
{
    tr_piece_index_t i, n;

    if ( tr_torrentIsSeed( tor ) )
        return FALSE;

    if( !tr_torrentIsPieceTransferAllowed( tor, TR_PEER_TO_CLIENT ) )
        return FALSE;

    for( i=0, n=tor->info.pieceCount; i<n; ++i )
        if( isPieceInteresting( tor, peer, i ) )
            return TRUE;

    return FALSE;
}

/* determines who we send "interested" messages to */
static void
rechokeDownloads( Torrent * t )
{
    int i;
    const uint64_t now = tr_date( );
    const int msec = 60 * 1000;
    const int MIN_INTERESTING_PEERS = 5;
    const int peerCount = tr_ptrArraySize( &t->peers );
    int maxPeers;

    int badCount         = 0;
    int goodCount        = 0;
    int untestedCount    = 0;
    tr_peer ** bad       = tr_new( tr_peer*, peerCount );
    tr_peer ** good      = tr_new( tr_peer*, peerCount );
    tr_peer ** untested  = tr_new( tr_peer*, peerCount );

    /* decide how many peers to be interested in */
    {
        int blocks = 0;
        int cancels = 0;

        /* Count up how many blocks & cancels each peer has.
         *
         * There are two situations where we send out cancels --
         *
         * 1. We've got unresponsive peers, which is handled by deciding
         *    -which- peers to be interested in.
         *
         * 2. We've hit our bandwidth cap, which is handled by deciding
         *    -how many- peers to be interested in.
         *
         * We're working on 2. here, so we need to ignore unresponsive
         * peers in our calculations lest they confuse Transmission into
         * thinking it's hit its bandwidth cap.
         */
        for( i=0; i<peerCount; ++i )
        {
            const tr_peer * peer = tr_ptrArrayNth( &t->peers, i );
            const int b = tr_historyGet( peer->blocksSentToClient, now, msec );
            const int c = tr_historyGet( peer->cancelsSentToPeer, now, msec );

            if( b == 0 ) /* ignore unresponsive peers, as described above */
                continue;

            blocks += b;
            cancels += c;
        }

        if( !t->interestedCount )
        {
            /* this is the torrent's first time to call this function...
             * start off optimistically by allowing interest in many peers */
            maxPeers = t->tor->maxConnectedPeers;
        }
        else if( !blocks )
        {
            /* we've gotten cancels but zero blocks...
             * something is seriously wrong.  throttle back sharply */
            maxPeers = t->interestedCount * 0.5;
        }
        else
        {
            const double cancelRate = cancels / (double)(cancels + blocks);
                 if( cancelRate >= 0.20 ) maxPeers = t->interestedCount * 0.7;
            else if( cancelRate >= 0.10 ) maxPeers = t->interestedCount * 0.8;
            else if( cancelRate >= 0.05 ) maxPeers = t->interestedCount * 0.9;
            else if( cancelRate >= 0.01 ) maxPeers = t->interestedCount;
            else                          maxPeers = t->interestedCount + 1;

            /* if things are getting worse, don't add more peers */
            if( ( t->cancelRate > 0.01 ) && ( cancelRate > t->cancelRate ) )
                maxPeers = MIN( maxPeers, t->interestedCount );

            t->cancelRate = cancelRate;

            tordbg( t, "cancel rate is %.3f -- changing the "
                       "number of peers we're interested in from %d to %d",
                       cancelRate, t->interestedCount, maxPeers );
        }
    }

    /* don't let the previous paragraph's number tweaking go too far... */
    if( maxPeers < MIN_INTERESTING_PEERS )
        maxPeers = MIN_INTERESTING_PEERS;
    if( maxPeers > t->tor->maxConnectedPeers )
        maxPeers = t->tor->maxConnectedPeers;

    /* separate the peers into "good" (ones with a low cancel-to-block ratio),
     * untested peers, and "bad" (ones with a high cancel-to-block ratio).
     * That's the order in which we'll choose who to show interest in */
    {
        /* Randomize the peer array so the peers in the three groups will be unsorted... */
        int n = peerCount;
        tr_peer ** peers = tr_memdup( tr_ptrArrayBase( &t->peers ), n * sizeof( tr_peer * ) );

        while( n > 0 )
        {
            const int i = tr_cryptoWeakRandInt( n );
            tr_peer * peer = tr_ptrArrayNth( &t->peers, i );

            if( !isPeerInteresting( t->tor, peer ) )
            {
                tr_peerMsgsSetInterested( peer->msgs, FALSE );
            }
            else
            {
                const int blocks = tr_historyGet( peer->blocksSentToClient, now, msec );
                const int cancels = tr_historyGet( peer->cancelsSentToPeer, now, msec );

                if( !blocks && !cancels )
                    untested[untestedCount++] = peer;
                else if( !cancels )
                    good[goodCount++] = peer;
                else if( !blocks )
                    bad[badCount++] = peer;
                else if( ( cancels * 10 ) < blocks )
                    good[goodCount++] = peer;
                else
                    bad[badCount++] = peer;
            }

            tr_removeElementFromArray( peers, i, sizeof(tr_peer*), n-- );
        }

        tr_free( peers );
    }

    t->interestedCount = 0;

    /* We've decided (1) how many peers to be interested in,
     * and (2) which peers are the best candidates,
     * Now it's time to update our `interest' flags. */
    for( i=0; i<goodCount; ++i ) {
        const tr_bool b = t->interestedCount < maxPeers;
        tr_peerMsgsSetInterested( good[i]->msgs, b );
        if( b )
            ++t->interestedCount;
    }
    for( i=0; i<untestedCount; ++i ) {
        const tr_bool b = t->interestedCount < maxPeers;
        tr_peerMsgsSetInterested( untested[i]->msgs, b );
        if( b )
            ++t->interestedCount;
    }
    for( i=0; i<badCount; ++i ) {
        const tr_bool b = t->interestedCount < maxPeers;
        tr_peerMsgsSetInterested( bad[i]->msgs, b );
        if( b )
            ++t->interestedCount;
    }

/*fprintf( stderr, "num interested: %d\n", t->interestedCount );*/

    /* cleanup */
    tr_free( untested );
    tr_free( good );
    tr_free( bad );
}

/**
***
**/

struct ChokeData
{
    tr_bool         doUnchoke;
    tr_bool         isInterested;
    tr_bool         isChoked;
    int             rate;
    int             salt;
    tr_peer *       peer;
};

static int
compareChoke( const void * va,
              const void * vb )
{
    const struct ChokeData * a = va;
    const struct ChokeData * b = vb;

    if( a->rate != b->rate ) /* prefer higher overall speeds */
        return a->rate > b->rate ? -1 : 1;

    if( a->isChoked != b->isChoked ) /* prefer unchoked */
        return a->isChoked ? 1 : -1;

    if( a->salt != b->salt ) /* random order */
        return a->salt - b->salt;

    return 0;
}

/* is this a new connection? */
static int
isNew( const tr_peer * peer )
{
    return peer && peer->io && tr_peerIoGetAge( peer->io ) < 45;
}

static void
rechokeUploads( Torrent * t, const uint64_t now )
{
    int i, size, unchokedInterested;
    const int peerCount = tr_ptrArraySize( &t->peers );
    tr_peer ** peers = (tr_peer**) tr_ptrArrayBase( &t->peers );
    struct ChokeData * choke = tr_new0( struct ChokeData, peerCount );
    const tr_session * session = t->manager->session;
    const int chokeAll = !tr_torrentIsPieceTransferAllowed( t->tor, TR_CLIENT_TO_PEER );

    assert( torrentIsLocked( t ) );

    /* sort the peers by preference and rate */
    for( i = 0, size = 0; i < peerCount; ++i )
    {
        tr_peer * peer = peers[i];
        struct peer_atom * atom = peer->atom;

        if( peer->progress >= 1.0 ) /* choke all seeds */
        {
            tr_peerMsgsSetChoke( peer->msgs, TRUE );
        }
        else if( atom->uploadOnly == UPLOAD_ONLY_YES ) /* choke partial seeds */
        {
            tr_peerMsgsSetChoke( peer->msgs, TRUE );
        }
        else if( chokeAll ) /* choke everyone if we're not uploading */
        {
            tr_peerMsgsSetChoke( peer->msgs, TRUE );
        }
        else
        {
            struct ChokeData * n = &choke[size++];
            n->peer         = peer;
            n->isInterested = peer->peerIsInterested;
            n->isChoked     = peer->peerIsChoked;
            n->rate         = tr_peerGetPieceSpeed( peer, now, TR_CLIENT_TO_PEER ) * 1024;
            n->salt         = tr_cryptoWeakRandInt( INT_MAX );
        }
    }

    qsort( choke, size, sizeof( struct ChokeData ), compareChoke );

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke.
     */
    unchokedInterested = 0;
    for( i=0; i<size && unchokedInterested<session->uploadSlotsPerTorrent; ++i ) {
        choke[i].doUnchoke = 1;
        if( choke[i].isInterested )
            ++unchokedInterested;
    }

    /* optimistic unchoke */
    if( i < size )
    {
        int n;
        struct ChokeData * c;
        tr_ptrArray randPool = TR_PTR_ARRAY_INIT;

        for( ; i<size; ++i )
        {
            if( choke[i].isInterested )
            {
                const tr_peer * peer = choke[i].peer;
                int x = 1, y;
                if( isNew( peer ) ) x *= 3;
                for( y=0; y<x; ++y )
                    tr_ptrArrayAppend( &randPool, &choke[i] );
            }
        }

        if(( n = tr_ptrArraySize( &randPool )))
        {
            c = tr_ptrArrayNth( &randPool, tr_cryptoWeakRandInt( n ));
            c->doUnchoke = 1;
            t->optimistic = c->peer;
        }

        tr_ptrArrayDestruct( &randPool, NULL );
    }

    for( i=0; i<size; ++i )
        tr_peerMsgsSetChoke( choke[i].peer->msgs, !choke[i].doUnchoke );

    /* cleanup */
    tr_free( choke );
}

static void
rechokePulse( int foo UNUSED, short bar UNUSED, void * vmgr )
{
    uint64_t now;
    tr_torrent * tor = NULL;
    tr_peerMgr * mgr = vmgr;
    managerLock( mgr );

    now = tr_date( );
    while(( tor = tr_torrentNext( mgr->session, tor ))) {
        if( tor->isRunning ) {
            rechokeUploads( tor->torrentPeers, now );
            if( !tr_torrentIsSeed( tor ) )
                rechokeDownloads( tor->torrentPeers );
        }
    }

    tr_timerAddMsec( mgr->rechokeTimer, RECHOKE_PERIOD_MSEC );
    managerUnlock( mgr );
}

/***
****
****  Life and Death
****
***/

typedef enum
{
    TR_CAN_KEEP,
    TR_CAN_CLOSE,
    TR_MUST_CLOSE,
}
tr_close_type_t;

static tr_close_type_t
shouldPeerBeClosed( const Torrent    * t,
                    const tr_peer    * peer,
                    int                peerCount,
                    const time_t       now )
{
    const tr_torrent *       tor = t->tor;
    const struct peer_atom * atom = peer->atom;

    /* if it's marked for purging, close it */
    if( peer->doPurge )
    {
        tordbg( t, "purging peer %s because its doPurge flag is set",
                tr_atomAddrStr( atom ) );
        return TR_MUST_CLOSE;
    }

    /* if we're seeding and the peer has everything we have,
     * and enough time has passed for a pex exchange, then disconnect */
    if( tr_torrentIsSeed( tor ) )
    {
        tr_bool peerHasEverything;

        if( atom->seedProbability != -1 )
        {
            peerHasEverything = atomIsSeed( atom );
        }
        else
        {
            tr_bitfield * tmp = tr_bitfieldDup( tr_cpPieceBitfield( &tor->completion ) );
            tr_bitsetDifference( tmp, &peer->have );
            peerHasEverything = tr_bitfieldCountTrueBits( tmp ) == 0;
            tr_bitfieldFree( tmp );
        }

        if( peerHasEverything && ( !tr_torrentAllowsPex(tor) || (now-atom->time>=30 )))
        {
            tordbg( t, "purging peer %s because we're both seeds",
                    tr_atomAddrStr( atom ) );
            return TR_MUST_CLOSE;
        }
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        const int relaxStrictnessIfFewerThanN = (int)( ( getMaxPeerCount( tor ) * 0.9 ) + 0.5 );
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        const float strictness = peerCount >= relaxStrictnessIfFewerThanN
                               ? 1.0
                               : peerCount / (float)relaxStrictnessIfFewerThanN;
        const int lo = MIN_UPLOAD_IDLE_SECS;
        const int hi = MAX_UPLOAD_IDLE_SECS;
        const int limit = hi - ( ( hi - lo ) * strictness );
        const int idleTime = now - MAX( atom->time, atom->piece_data_time );
/*fprintf( stderr, "strictness is %.3f, limit is %d seconds... time since connect is %d, time since piece is %d ... idleTime is %d, doPurge is %d\n", (double)strictness, limit, (int)(now - atom->time), (int)(now - atom->piece_data_time), idleTime, idleTime > limit );*/
        if( idleTime > limit ) {
            tordbg( t, "purging peer %s because it's been %d secs since we shared anything",
                       tr_atomAddrStr( atom ), idleTime );
            return TR_CAN_CLOSE;
        }
    }

    return TR_CAN_KEEP;
}

static void sortPeersByLivelinessReverse( tr_peer ** peers, void ** clientData, int n, uint64_t now );

static tr_peer **
getPeersToClose( Torrent * t, tr_close_type_t closeType, const time_t now, int * setmeSize )
{
    int i, peerCount, outsize;
    tr_peer ** peers = (tr_peer**) tr_ptrArrayPeek( &t->peers, &peerCount );
    struct tr_peer ** ret = tr_new( tr_peer *, peerCount );

    assert( torrentIsLocked( t ) );

    for( i = outsize = 0; i < peerCount; ++i )
        if( shouldPeerBeClosed( t, peers[i], peerCount, now ) == closeType )
            ret[outsize++] = peers[i];

    sortPeersByLivelinessReverse ( ret, NULL, outsize, tr_date( ) );

    *setmeSize = outsize;
    return ret;
}

static int
getReconnectIntervalSecs( const struct peer_atom * atom, const time_t now )
{
    int sec;

    /* if we were recently connected to this peer and transferring piece
     * data, try to reconnect to them sooner rather that later -- we don't
     * want network troubles to get in the way of a good peer. */
    if( ( now - atom->piece_data_time ) <= ( MINIMUM_RECONNECT_INTERVAL_SECS * 2 ) )
        sec = MINIMUM_RECONNECT_INTERVAL_SECS;

    /* don't allow reconnects more often than our minimum */
    else if( ( now - atom->time ) < MINIMUM_RECONNECT_INTERVAL_SECS )
        sec = MINIMUM_RECONNECT_INTERVAL_SECS;

    /* otherwise, the interval depends on how many times we've tried
     * and failed to connect to the peer */
    else switch( atom->numFails ) {
        case 0: sec = 0; break;
        case 1: sec = 5; break;
        case 2: sec = 2 * 60; break;
        case 3: sec = 15 * 60; break;
        case 4: sec = 30 * 60; break;
        case 5: sec = 60 * 60; break;
        default: sec = 120 * 60; break;
    }

    /* penalize peers that were unreachable the last time we tried */
    if( atom->myflags & MYFLAG_UNREACHABLE )
        sec += sec;

    dbgmsg( "reconnect interval for %s is %d seconds", tr_atomAddrStr( atom ), sec );
    return sec;
}

static void
closePeer( Torrent * t, tr_peer * peer )
{
    struct peer_atom * atom;

    assert( t != NULL );
    assert( peer != NULL );

    atom = peer->atom;

    /* if we transferred piece data, then they might be good peers,
       so reset their `numFails' weight to zero.  otherwise we connected
       to them fruitlessly, so mark it as another fail */
    if( atom->piece_data_time ) {
        tordbg( t, "resetting atom %s numFails to 0", tr_atomAddrStr(atom) );
        atom->numFails = 0;
    } else {
        ++atom->numFails;
        tordbg( t, "incremented atom %s numFails to %d", tr_atomAddrStr(atom), (int)atom->numFails );
    }

    tordbg( t, "removing bad peer %s", tr_peerIoGetAddrStr( peer->io ) );
    removePeer( t, peer );
}

static void
closeBadPeers( Torrent * t )
{
    const time_t  now = tr_time( );

    if( !t->isRunning )
    {
        removeAllPeers( t );
    }
    else
    {
        int i;
        int mustCloseCount;
        struct tr_peer ** mustClose;

        /* disconnect the really bad peers */
        mustClose = getPeersToClose( t, TR_MUST_CLOSE, now, &mustCloseCount );
        for( i=0; i<mustCloseCount; ++i )
            closePeer( t, mustClose[i] );
        tr_free( mustClose );
    }
}

struct peer_liveliness
{
    tr_peer * peer;
    void * clientData;
    time_t pieceDataTime;
    time_t time;
    int speed;
    tr_bool doPurge;
};

static int
comparePeerLiveliness( const void * va, const void * vb )
{
    const struct peer_liveliness * a = va;
    const struct peer_liveliness * b = vb;

    if( a->doPurge != b->doPurge )
        return a->doPurge ? 1 : -1;

    if( a->speed != b->speed ) /* faster goes first */
        return a->speed > b->speed ? -1 : 1;

    /* the one to give us data more recently goes first */
    if( a->pieceDataTime != b->pieceDataTime )
        return a->pieceDataTime > b->pieceDataTime ? -1 : 1;

    /* the one we connected to most recently goes first */
    if( a->time != b->time )
        return a->time > b->time ? -1 : 1;

    return 0;
}

static int
comparePeerLivelinessReverse( const void * va, const void * vb )
{
    return -comparePeerLiveliness (va, vb);
}

static void
sortPeersByLivelinessImpl( tr_peer  ** peers,
                           void     ** clientData,
                           int         n,
                           uint64_t    now,
                           int (*compare) ( const void *va, const void *vb ) )
{
    int i;
    struct peer_liveliness *lives, *l;

    /* build a sortable array of peer + extra info */
    lives = l = tr_new0( struct peer_liveliness, n );
    for( i=0; i<n; ++i, ++l )
    {
        tr_peer * p = peers[i];
        l->peer = p;
        l->doPurge = p->doPurge;
        l->pieceDataTime = p->atom->piece_data_time;
        l->time = p->atom->time;
        l->speed = 1024.0 * (   tr_peerGetPieceSpeed( p, now, TR_UP )
                              + tr_peerGetPieceSpeed( p, now, TR_DOWN ) );
        if( clientData )
            l->clientData = clientData[i];
    }

    /* sort 'em */
    assert( n == ( l - lives ) );
    qsort( lives, n, sizeof( struct peer_liveliness ), compare );

    /* build the peer array */
    for( i=0, l=lives; i<n; ++i, ++l ) {
        peers[i] = l->peer;
        if( clientData )
            clientData[i] = l->clientData;
    }
    assert( n == ( l - lives ) );

    /* cleanup */
    tr_free( lives );
}

static void
sortPeersByLiveliness( tr_peer ** peers, void ** clientData, int n, uint64_t now )
{
    sortPeersByLivelinessImpl( peers, clientData, n, now, comparePeerLiveliness );
}

static void
sortPeersByLivelinessReverse( tr_peer ** peers, void ** clientData, int n, uint64_t now )
{
    sortPeersByLivelinessImpl( peers, clientData, n, now, comparePeerLivelinessReverse );
}


static void
enforceTorrentPeerLimit( Torrent * t, uint64_t now )
{
    int n = tr_ptrArraySize( &t->peers );
    const int max = tr_torrentGetPeerLimit( t->tor );
    if( n > max )
    {
        void * base = tr_ptrArrayBase( &t->peers );
        tr_peer ** peers = tr_memdup( base, n*sizeof( tr_peer* ) );
        sortPeersByLiveliness( peers, NULL, n, now );
        while( n > max )
            closePeer( t, peers[--n] );
        tr_free( peers );
    }
}

static void
enforceSessionPeerLimit( tr_session * session, uint64_t now )
{
    int n = 0;
    tr_torrent * tor = NULL;
    const int max = tr_sessionGetPeerLimit( session );

    /* count the total number of peers */
    while(( tor = tr_torrentNext( session, tor )))
        n += tr_ptrArraySize( &tor->torrentPeers->peers );

    /* if there are too many, prune out the worst */
    if( n > max )
    {
        tr_peer ** peers = tr_new( tr_peer*, n );
        Torrent ** torrents = tr_new( Torrent*, n );

        /* populate the peer array */
        n = 0;
        tor = NULL;
        while(( tor = tr_torrentNext( session, tor ))) {
            int i;
            Torrent * t = tor->torrentPeers;
            const int tn = tr_ptrArraySize( &t->peers );
            for( i=0; i<tn; ++i, ++n ) {
                peers[n] = tr_ptrArrayNth( &t->peers, i );
                torrents[n] = t;
            }
        }

        /* sort 'em */
        sortPeersByLiveliness( peers, (void**)torrents, n, now );

        /* cull out the crappiest */
        while( n-- > max )
            closePeer( torrents[n], peers[n] );

        /* cleanup */
        tr_free( torrents );
        tr_free( peers );
    }
}

static void makeNewPeerConnections( tr_peerMgr * mgr, const int max );

static void
reconnectPulse( int foo UNUSED, short bar UNUSED, void * vmgr )
{
    tr_torrent * tor;
    tr_peerMgr * mgr = vmgr;
    const uint64_t now = tr_date( );

    /**
    ***  enforce the per-session and per-torrent peer limits
    **/

    /* if we're over the per-torrent peer limits, cull some peers */
    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor )))
        if( tor->isRunning )
            enforceTorrentPeerLimit( tor->torrentPeers, now );

    /* if we're over the per-session peer limits, cull some peers */
    enforceSessionPeerLimit( mgr->session, now );

    /* remove crappy peers */
    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor )))
        closeBadPeers( tor->torrentPeers );

    /* try to make new peer connections */
    makeNewPeerConnections( mgr, MAX_CONNECTIONS_PER_PULSE );
}

/****
*****
*****  BANDWIDTH ALLOCATION
*****
****/

static void
pumpAllPeers( tr_peerMgr * mgr )
{
    tr_torrent * tor = NULL;

    while(( tor = tr_torrentNext( mgr->session, tor )))
    {
        int j;
        Torrent * t = tor->torrentPeers;

        for( j=0; j<tr_ptrArraySize( &t->peers ); ++j )
        {
            tr_peer * peer = tr_ptrArrayNth( &t->peers, j );
            tr_peerMsgsPulse( peer->msgs );
        }
    }
}

static void
bandwidthPulse( int foo UNUSED, short bar UNUSED, void * vmgr )
{
    tr_torrent * tor;
    tr_peerMgr * mgr = vmgr;
    managerLock( mgr );

    /* FIXME: this next line probably isn't necessary... */
    pumpAllPeers( mgr );

    /* allocate bandwidth to the peers */
    tr_bandwidthAllocate( mgr->session->bandwidth, TR_UP, BANDWIDTH_PERIOD_MSEC );
    tr_bandwidthAllocate( mgr->session->bandwidth, TR_DOWN, BANDWIDTH_PERIOD_MSEC );

    /* possibly stop torrents that have seeded enough */
    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor )))
        tr_torrentCheckSeedRatio( tor );

    /* run the completeness check for any torrents that need it */
    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor ))) {
        if( tor->torrentPeers->needsCompletenessCheck ) {
            tor->torrentPeers->needsCompletenessCheck  = FALSE;
            tr_torrentRecheckCompleteness( tor );
        }
    }

    /* stop torrents that are ready to stop, but couldn't be stopped earlier
    * during the peer-io callback call chain */
    tor = NULL;
    while(( tor = tr_torrentNext( mgr->session, tor )))
        if( tor->isStopping )
            tr_torrentStop( tor );

    reconnectPulse( 0, 0, mgr );

    tr_timerAddMsec( mgr->bandwidthTimer, BANDWIDTH_PERIOD_MSEC );
    managerUnlock( mgr );
}

/***
****
***/

static int
compareAtomPtrsByAddress( const void * va, const void *vb )
{
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;

    assert( tr_isAtom( a ) );
    assert( tr_isAtom( b ) );

    return tr_compareAddresses( &a->addr, &b->addr );
}

/* best come first, worst go last */
static int
compareAtomPtrsByShelfDate( const void * va, const void *vb )
{
    time_t atime;
    time_t btime;
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;
    const int data_time_cutoff_secs = 60 * 60;
    const time_t tr_now = tr_time( );

    assert( tr_isAtom( a ) );
    assert( tr_isAtom( b ) );

    /* primary key: the last piece data time *if* it was within the last hour */
    atime = a->piece_data_time; if( atime + data_time_cutoff_secs < tr_now ) atime = 0;
    btime = b->piece_data_time; if( btime + data_time_cutoff_secs < tr_now ) btime = 0;
    if( atime != btime )
        return atime > btime ? -1 : 1;

    /* secondary key: shelf date. */
    if( a->shelf_date != b->shelf_date )
        return a->shelf_date > b->shelf_date ? -1 : 1;

    return 0;
}

static int
getMaxAtomCount( const tr_torrent * tor )
{
    /* FIXME: this curve should be smoother... */
    const int n = tor->maxConnectedPeers;
    if( n >= 200 ) return n * 1.5;
    if( n >= 100 ) return n * 2;
    if( n >=  50 ) return n * 3;
    if( n >=  20 ) return n * 5;
    return n * 10;
}

static void
atomPulse( int foo UNUSED, short bar UNUSED, void * vmgr )
{
    tr_torrent * tor = NULL;
    tr_peerMgr * mgr = vmgr;
    managerLock( mgr );

    while(( tor = tr_torrentNext( mgr->session, tor )))
    {
        int atomCount;
        Torrent * t = tor->torrentPeers;
        const int maxAtomCount = getMaxAtomCount( tor );
        struct peer_atom ** atoms = (struct peer_atom**) tr_ptrArrayPeek( &t->pool, &atomCount );

        if( atomCount > maxAtomCount ) /* we've got too many atoms... time to prune */
        {
            int i;
            int keepCount = 0;
            int testCount = 0;
            struct peer_atom ** keep = tr_new( struct peer_atom*, atomCount );
            struct peer_atom ** test = tr_new( struct peer_atom*, atomCount );

            /* keep the ones that are in use */
            for( i=0; i<atomCount; ++i ) {
                struct peer_atom * atom = atoms[i];
                if( peerIsInUse( t, atom ) )
                    keep[keepCount++] = atom;
                else
                    test[testCount++] = atom;
            }

            /* if there's room, keep the best of what's left */
            i = 0;
            if( keepCount < maxAtomCount ) {
                qsort( test, testCount, sizeof( struct peer_atom * ), compareAtomPtrsByShelfDate );
                while( i<testCount && keepCount<maxAtomCount )
                    keep[keepCount++] = test[i++];
            }

            /* free the culled atoms */
            while( i<testCount )
                tr_free( test[i++] );

            /* rebuild Torrent.pool with what's left */
            tr_ptrArrayDestruct( &t->pool, NULL );
            t->pool = TR_PTR_ARRAY_INIT;
            qsort( keep, keepCount, sizeof( struct peer_atom * ), compareAtomPtrsByAddress );
            for( i=0; i<keepCount; ++i )
                tr_ptrArrayAppend( &t->pool, keep[i] );

            tordbg( t, "max atom count is %d... pruned from %d to %d\n", maxAtomCount, atomCount, keepCount );

            /* cleanup */
            tr_free( test );
            tr_free( keep );
        }
    }

    tr_timerAddMsec( mgr->atomTimer, ATOM_PERIOD_MSEC );
    managerUnlock( mgr );
}

/***
****
****
****
***/

static inline tr_bool
isBandwidthMaxedOut( const tr_bandwidth * b,
                     const uint64_t now_msec, tr_direction dir )
{
    if( !tr_bandwidthIsLimited( b, dir ) )
        return FALSE;
    else {
        const double got = tr_bandwidthGetPieceSpeed( b, now_msec, dir );
        const double want = tr_bandwidthGetDesiredSpeed( b, dir );
        return got >= want;
    }
}

/* is this atom someone that we'd want to initiate a connection to? */
static tr_bool
isPeerCandidate( const tr_torrent * tor, struct peer_atom * atom, const time_t now )
{
    /* not if they're banned... */
    if( atom->myflags & MYFLAG_BANNED )
        return FALSE;

    /* not if we're both seeds */
    if( tr_torrentIsSeed( tor ) )
        if( atomIsSeed( atom ) || ( atom->uploadOnly == UPLOAD_ONLY_YES ) )
            return FALSE;
 
    /* not if we just tried them already */
    if( ( now - atom->time ) < getReconnectIntervalSecs( atom, now ) )
        return FALSE;

    /* not if they're blocklisted */
    if( isAtomBlocklisted( tor->session, atom ) )
        return FALSE;

    /* not if we've already got a connection to them...  */
    if( peerIsInUse( tor->torrentPeers, atom ) )
        return FALSE;

    return TRUE;
}

struct peer_candidate
{
    uint64_t score;
    tr_torrent * tor;
    struct peer_atom * atom;
};

static tr_bool
torrentWasRecentlyStarted( const tr_torrent * tor )
{
    return difftime( tr_time( ), tor->startDate ) < 120;
}

static inline uint64_t
addValToKey( uint64_t value, int width, uint64_t addme )
{
    value = (value << (uint64_t)width);
    value |= addme;
    return value;
}

/* smaller value is better */
static uint64_t
getPeerCandidateScore( const tr_torrent * tor, const struct peer_atom * atom, uint8_t salt  )
{
    uint64_t i;
    uint64_t score = 0;
    const tr_bool failed = atom->lastConnectionAt < atom->lastConnectionAttemptAt;

    /* prefer peers we've connected to, or never tried, over peers we failed to connect to. */
    i = failed ? 1 : 0;
    score = addValToKey( score, 1, i );

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = atom->lastConnectionAttemptAt;
    score = addValToKey( score, 32, i );

    /* prefer peers belonging to a torrent of a higher priority */
    switch( tr_torrentGetPriority( tor ) ) {
        case TR_PRI_HIGH:    i = 0; break;
        case TR_PRI_NORMAL:  i = 1; break;
        case TR_PRI_LOW:     i = 2; break;
    }
    score = addValToKey( score, 4, i );

    /* prefer recently-started torrents */
    i = torrentWasRecentlyStarted( tor ) ? 0 : 1;
    score = addValToKey( score, 1, i );

    /* prefer torrents we're downloading with */
    i = tr_torrentIsSeed( tor ) ? 1 : 0;
    score = addValToKey( score, 1, i );

    /* prefer peers that we might have a chance of uploading to...
       so lower seed probability is better */
    if( atom->seedProbability == 100 ) i = 101;
    else if( atom->seedProbability == -1 ) i = 100;
    else i = atom->seedProbability;
    score = addValToKey( score, 8, i );

    /* Prefer peers that we got from more trusted sources.
     * lower `from' values indicate more trusted sources */
    score = addValToKey( score, 4, atom->from );

    /* salt */
    score = addValToKey( score, 8, salt );

    return score;
}

/* sort an array of peer candidates */
static int
comparePeerCandidates( const void * va, const void * vb )
{
    const struct peer_candidate * a = va;
    const struct peer_candidate * b = vb;

    if( a->score < b->score ) return -1;
    if( a->score > b->score ) return 1;

    return 0;
}

/** @return an array of all the atoms we might want to connect to */
static struct peer_candidate*
getPeerCandidates( tr_session * session, int * candidateCount )
{
    int n;
    tr_torrent * tor;
    struct peer_candidate * candidates;
    struct peer_candidate * walk;
    const time_t now = tr_time( );
    const uint64_t now_msec = tr_date( );
    /* leave 5% of connection slots for incoming connections -- ticket #2609 */
    const int maxCandidates = tr_sessionGetPeerLimit( session ) * 0.95;

    /* don't start any new handshakes if we're full up */
    n = 0;
    tor= NULL;
    while(( tor = tr_torrentNext( session, tor )))
        n += tr_ptrArraySize( &tor->torrentPeers->peers );
    if( maxCandidates <= n ) {
        *candidateCount = 0;
        return NULL;
    }

    /* allocate an array of candidates */
    n = 0;
    tor= NULL;
    while(( tor = tr_torrentNext( session, tor )))
        n += tr_ptrArraySize( &tor->torrentPeers->pool );
    walk = candidates = tr_new( struct peer_candidate, n );

    /* populate the candidate array */
    tor = NULL;
    while(( tor = tr_torrentNext( session, tor )))
    {
        int i, nAtoms;
        struct peer_atom ** atoms;

        if( !tor->torrentPeers->isRunning )
            continue;

        /* if we've already got enough peers in this torrent... */
        if( tr_torrentGetPeerLimit( tor ) <= tr_ptrArraySize( &tor->torrentPeers->peers ) )
            continue;

        /* if we've already got enough speed in this torrent... */
        if( tr_torrentIsSeed( tor ) && isBandwidthMaxedOut( tor->bandwidth, now_msec, TR_UP ) )
            continue;

        atoms = (struct peer_atom**) tr_ptrArrayPeek( &tor->torrentPeers->pool, &nAtoms );
        for( i=0; i<nAtoms; ++i )
        {
            struct peer_atom * atom = atoms[i];

            if( isPeerCandidate( tor, atom, now ) )
            {
                const uint8_t salt = tr_cryptoWeakRandInt( 1024 );
                walk->tor = tor;
                walk->atom = atom;
                walk->score = getPeerCandidateScore( tor, atom, salt );
                ++walk;
            }
        }
    }

    *candidateCount = walk - candidates;
    if( *candidateCount > 1 )
        qsort( candidates, *candidateCount, sizeof( struct peer_candidate ), comparePeerCandidates );
    return candidates;
}

static void
initiateConnection( tr_peerMgr * mgr, Torrent * t, struct peer_atom * atom )
{
    tr_peerIo * io;
    const time_t now = tr_time( );

    tordbg( t, "Starting an OUTGOING connection with %s", tr_atomAddrStr( atom ) );

    io = tr_peerIoNewOutgoing( mgr->session,
                               mgr->session->bandwidth,
                               &atom->addr,
                               atom->port,
                               t->tor->info.hash,
                               t->tor->completeness == TR_SEED );

    if( io == NULL )
    {
        tordbg( t, "peerIo not created; marking peer %s as unreachable",
                tr_atomAddrStr( atom ) );
        atom->myflags |= MYFLAG_UNREACHABLE;
        atom->numFails++;
    }
    else
    {
        tr_handshake * handshake = tr_handshakeNew( io,
                                                    mgr->session->encryptionMode,
                                                    myHandshakeDoneCB,
                                                    mgr );

        assert( tr_peerIoGetTorrentHash( io ) );

        tr_peerIoUnref( io ); /* balanced by the initial ref
                                 in tr_peerIoNewOutgoing() */

        tr_ptrArrayInsertSorted( &t->outgoingHandshakes, handshake,
                                 handshakeCompare );
    }

    atom->lastConnectionAttemptAt = now;
    atom->time = now;
}

static void
initiateCandidateConnection( tr_peerMgr * mgr, struct peer_candidate * c )
{
#if 0
    fprintf( stderr, "Starting an OUTGOING connection with %s - [%s] seedProbability==%d; %s, %s\n",
             tr_atomAddrStr( c->atom ),
             tr_torrentName( c->tor ),
             (int)c->atom->seedProbability,
             tr_torrentIsPrivate( c->tor ) ? "private" : "public",
             tr_torrentIsSeed( c->tor ) ? "seed" : "downloader" );
#endif

    initiateConnection( mgr, c->tor->torrentPeers, c->atom );
}

static void
makeNewPeerConnections( struct tr_peerMgr * mgr, const int max )
{
    int i, n;
    struct peer_candidate * candidates;

    candidates = getPeerCandidates( mgr->session, &n );

    for( i=0; i<n && i<max; ++i )
        initiateCandidateConnection( mgr, &candidates[i] );

    tr_free( candidates );
}


/**
 * Replication count ( for rarest first policy )
 */


void
tr_incrReplicationOfPiece( Torrent * t, const size_t index )
{
    /* One more replication of this piece is present in the swarm */
    ++t->pieceReplication[index];

    /* we only resort the piece if the list is already sorted */
    if( tr_isRarestListSorted( t ) )
        pieceListResortPiece( t, index );

}

void
tr_incrReplicationFromBitset( Torrent * t, const tr_bitset * bitset )
{
    if( bitset->haveAll )
    {
        tr_incrReplication( t );
    }
    else if ( !bitset->haveNone )
    {
        tr_incrReplicationFromBitfield( t, &bitset->bitfield );
    }
}

void
tr_incrReplication( Torrent * t )
{
    uint32_t it;

    for( it=0 ; it < t->tor->info.pieceCount ; it++ )
        ++t->pieceReplication[it];
}

void
tr_incrReplicationFromBitfield( Torrent * t, const tr_bitfield * bitfield )
{
    uint32_t it;

    assert( bitfield->bitCount == t->tor->info.pieceCount );
    assert( tr_bitfieldTestFast( bitfield, t->tor->info.pieceCount - 1 ) );

    for( it=0 ; it < t->tor->info.pieceCount ; it++ )
    {
        if( tr_bitfieldHasFast( bitfield, it ) )
            ++t->pieceReplication[it];
    }
    tr_setRarestListSorted( t, FALSE );
}

void
tr_decrReplicationFromBitset( Torrent * t, const tr_bitset * bitset )
{
    uint32_t it;

    if( bitset->haveAll )
    {
        for( it=0 ; it < t->tor->info.pieceCount ; ++it )
            --t->pieceReplication[it];
    }
    else if ( !bitset->haveNone )
    {
        for( it=0 ; it < bitset->bitfield.bitCount ; it++ )
        {
            if( tr_bitfieldHasFast( &bitset->bitfield, it ) )
                --t->pieceReplication[it];
        }
        tr_setRarestListSorted( t, FALSE );
    }

}


/**
 * Weighted pieces ( definition )
 */


static void
pieceListRebuild( Torrent * t )
{

    if( !tr_torrentIsSeed( t->tor ) )
    {
        tr_piece_index_t i;
        tr_piece_index_t * pool;
        tr_piece_index_t poolCount = 0;
        const tr_torrent * tor = t->tor;
        const tr_info * inf = tr_torrentInfo( tor );
        struct weighted_piece * pieces;

        /* build the new list */
        pool = tr_new( tr_piece_index_t, inf->pieceCount );
        pieces = tr_new0( struct weighted_piece, inf->pieceCount );

        /* every piece has a corresponding weighted_piece structure */
        for( i=0; i<inf->pieceCount; ++i )
        {
            struct weighted_piece * piece = pieces + i;
            piece->index = i;
            piece->requestCount = 0;
            piece->salt = tr_cryptoWeakRandInt( 4096 );
            piece->maxDup = 1;

            /* track the pieces we will need to download */
            if( !inf->pieces[i].dnd )
                if( !tr_cpPieceIsComplete( &tor->completion, i ) )
                    pool[poolCount++] = i;
        }

        /* if we already had a list of pieces, merge it into
         * the new list so we don't lose its requestCounts */
        if( t->pieces != NULL )
        {
            struct weighted_piece * o = t->pieces;
            struct weighted_piece * oend = o + t->pieceCount;
            struct weighted_piece * n = pieces;
            struct weighted_piece * nend = n + inf->pieceCount;

            while( o!=oend && n!=nend ) {
                if( o->index < n->index )
                    ++o;
                else if( o->index > n->index )
                    ++n;
                else
                    *n++ = *o++;
            }

            tr_free( t->pieces );
        }

        t->pieces = pieces;
        t->pieceCount = inf->pieceCount;

        if( tr_torIsZipf( t->tor ) )
            updateZipfProbabilities( t );

        if( t->piecesOrder != NULL )
            tr_free( t->piecesOrder );

        if( t->piecesRarest != NULL )
            tr_free( t->piecesRarest );

        /* Only the pieces still to download are in the sorted lists */
        t->pieceCountOrder = 0;
        t->pieceCountRarest = 0;
        t->piecesOrder = tr_new( tr_piece_index_t, poolCount);
        t->piecesRarest= tr_new( tr_piece_index_t, poolCount);

        for( i=0; i<poolCount; ++i ) {
            insertInSortedLists( t, pool[i] );
        }

        assertWeightedListsAreConsistent( t );

        /* cleanup */
        tr_free( pool );
    }
}

static void
pieceListRemovePiece( Torrent * t, tr_piece_index_t piece )
{
    const int posRarest = piecePosInRarest( t, piece );
    const int posOrder = piecePosInOrder( t, piece );

    /* Make sure the piece is present in both array or in none of them */
    //assert( posOrder != posRarest && (posOrder == -1 || posRarest == -1) );
    assert( (posOrder * posRarest) >= 0 );
    if( posOrder != -1 && posRarest != -1 )
    {
        tr_removeElementFromArray( t->piecesOrder,
                                   posOrder,
                                   sizeof( tr_piece_index_t ),
                                   t->pieceCountOrder-- );
        tr_removeElementFromArray( t->piecesRarest,
                                   posRarest,
                                   sizeof( tr_piece_index_t ),
                                   t->pieceCountRarest-- );

        assert( t->pieceCountRarest == t->pieceCountOrder );

        if( t->pieceCountRarest == 0 )
        {
            tr_free( t->piecesOrder );
            t->piecesOrder = NULL;
            tr_free( t->piecesRarest );
            t->piecesRarest = NULL;
        }

    }

}

static void
pieceListRemoveRequest( Torrent * t, tr_block_index_t block )
{
    struct weighted_piece * p;
    const tr_piece_index_t index = tr_torBlockPiece( t->tor, block );

    if( ((p = pieceListLookup( t, index ))) && ( p->requestCount > 0 ) )
    {
        --p->requestCount;
        pieceListResortPiece( t, index );
    }
}

int
piecePosInOrder( Torrent * t, tr_piece_index_t index )
{
    int i;

    for( i=0; i<t->pieceCountOrder; ++i )
        if( t->piecesOrder[i] == index )
            return i;

    return -1;
}

int
piecePosInRarest( Torrent * t, tr_piece_index_t index )
{
    int i;

    for( i=0; i<t->pieceCountRarest; ++i )
        if( t->piecesRarest[i] == index )
            return i;

    return -1;
}

void
pieceListResortPiece( Torrent * t, tr_piece_index_t index )
{
    pieceListOrderResortPiece( t, index );
    pieceListRarestResortPiece( t, index );
}


void
pieceListRarestResortPiece( Torrent * t, tr_piece_index_t index )
{
    int pos;
    tr_bool isSorted = TRUE;

    pos = piecePosInRarest( t, index );

    /* the piee is not in the list */
    if( pos == -1 )
        return;

    weightTorrent = t->tor;
    if( isSorted && ( pos > 0 ) && ( comparePieceByRareness( t->piecesRarest+pos-1, t->piecesRarest+pos ) > 0 ) )
        isSorted = FALSE;
    if( isSorted && ( pos < t->pieceCountRarest - 1 ) && ( comparePieceByRareness( t->piecesRarest+pos, t->piecesRarest+pos+1 ) > 0 ) )
        isSorted = FALSE;

    if( !tr_isRarestListSorted( t ) )
    {
       pieceListRarestSort( t );
       isSorted = TRUE;
    }

    /* if it's not sorted, move it around */
    if( !isSorted )
    {
        tr_bool exact;

        tr_removeElementFromArray( t->piecesRarest,
                                   pos,
                                   sizeof( tr_piece_index_t ),
                                   t->pieceCountRarest );
         t->pieceCountRarest--;

        pos = tr_lowerBound( &index, t->piecesRarest, t->pieceCountRarest,
                             sizeof( tr_piece_index_t ),
                             comparePieceByRareness, &exact );

        memmove( &t->piecesRarest[pos + 1],
                 &t->piecesRarest[pos],
                 sizeof( tr_piece_index_t ) * ( t->pieceCountRarest - pos ) );
        t->pieceCountRarest++;
        t->piecesRarest[pos] = index;
    }

    assertRarestPiecesAreSorted( t );
}

void
pieceListOrderResortPiece( Torrent * t, tr_piece_index_t index )
{

    int pos;
    tr_bool isSorted = TRUE;

    pos = piecePosInOrder( t, index );

    /* the piece is not in the list */
    if( pos == -1 )
        return;

    weightTorrent = t->tor;
    if( isSorted && ( pos > 0 ) && ( comparePieceByOrder( t->piecesOrder+pos-1, t->piecesOrder+pos ) > 0 ) )
        isSorted = FALSE;
    if( isSorted && ( pos < t->pieceCountOrder - 1 ) && ( comparePieceByOrder( t->piecesOrder+pos, t->piecesOrder+pos+1 ) > 0 ) )
        isSorted = FALSE;

    /* if it's not sorted, move it around */
    if( !isSorted )
    {
        tr_bool exact;

        tr_removeElementFromArray( t->piecesOrder,
                                   pos,
                                   sizeof( tr_piece_index_t ),
                                   t->pieceCountOrder );
        t->pieceCountOrder--;

        pos = tr_lowerBound( &index, t->piecesOrder, t->pieceCountOrder,
                             sizeof( tr_piece_index_t ),
                             comparePieceByOrder, &exact );

        memmove( &t->piecesOrder[pos + 1],
                 &t->piecesOrder[pos],
                 sizeof( tr_piece_index_t ) * ( t->pieceCountOrder - pos ) );

        t->pieceCountOrder++;
        t->piecesOrder[pos] = index;
    }
    assertOrderPiecesAreSorted( t );
}

void
removeFromSortedLists( Torrent * t, tr_piece_index_t index)
{
    /* relative position of the piece in the regular/inOrder list */
    const int posRar = piecePosInRarest( t, index );
    const int posOrd = piecePosInOrder( t, index );

    tr_removeElementFromArray( t->piecesRarest,
                               posRar, 
                               sizeof( tr_piece_index_t ),
                               t->pieceCountRarest--);

    tr_removeElementFromArray( t->piecesOrder,
                               posOrd,
                               sizeof( tr_piece_index_t ),
                               t->pieceCountOrder--);
}

void insertInSortedLists( Torrent * t, tr_piece_index_t index )
{
    int pos;
    tr_bool exact;
 
    assertWeightedPiecesAreSorted( t );

    weightTorrent = t->tor;
    /* rarest list */
    pos = tr_lowerBound( &index, t->piecesRarest, t->pieceCountRarest,
                         sizeof( tr_piece_index_t ),
                         comparePieceByRareness, &exact );

    memmove( &t->piecesRarest[pos + 1],
             &t->piecesRarest[pos],
             sizeof( tr_piece_index_t ) * ( t->pieceCountRarest - pos ) );
    t->pieceCountRarest++;

    t->piecesRarest[pos] = index;

    weightTorrent = t->tor;
    /* inOrder List */
    pos = tr_lowerBound( &index, t->piecesOrder, t->pieceCountOrder,
                         sizeof( tr_piece_index_t ),
                         comparePieceByOrder, &exact );

    memmove( &t->piecesOrder[pos + 1],
             &t->piecesOrder[pos],
             sizeof( tr_piece_index_t ) * ( t->pieceCountOrder - pos ) );
    t->pieceCountOrder++;

    t->piecesOrder[pos] = index;

    assertWeightedPiecesAreSorted( t );
}


/**
 *   Zipf Related functions
 */

static void
updateZipfProbabilities( Torrent * t )
{
    int it;
    const float firstMissing = (float) tr_cpNextInOrdrerPiece( &t->tor->completion );
    const float power = t->tor->session->zipfTeta;

    for( it=(int)firstMissing ; it<t->pieceCount ; it++ )
        t->pieces[it].salt = 1/ ( pow(it + 1 - firstMissing, power) );
}

/* The pieces are divided in three categories :
 1st : already downloaded/requested pieces
 2nd : normal priority pieces
 3rd : completely downloaded/requested pieces
 THis function return an array in which the pieces of the second part are ordered
 according to the zipf policy :
 "The probability of selecting each of these pieces is chosen to be proportionnal to
 1/(k+1- k0)^teta where k is the index of the piece and k0 is the index of the
 first missing piece."
 */
static tr_piece_index_t *
createZipfPiecesArray( const Torrent * t, const tr_peer * peer, int * length )
{
    tr_piece_index_t * array;
    int it;
    int startUnorderedPieces, endUnorderedPieces;
    float sum;

    *length = 0;

    if( t->pieceCountOrder == 0 )
        return NULL;

    array = tr_new( tr_piece_index_t, t->pieceCountOrder );

    startUnorderedPieces = -1;
    endUnorderedPieces = -1;

    /* copy the pieces we want and that the peer has to the new array.
     * Mark the beginning and end of the pieces on which we will apply zipf */
    for( it=0 ; it < t->pieceCountOrder ; it++ )
    {
        const struct weighted_piece * p = pieceListLookup( t, t->piecesOrder[it] );
        const int missing = tr_cpMissingBlocksInPiece( &t->tor->completion, p->index );
        const int nbBlocks = (int) tr_torPieceCountBlocks( t->tor, p->index );

        /* Only treat a piece if the peer has it */
        if( !tr_bitsetHas( &peer->have, p->index ) )
            continue;

        if( p->requestCount == 0 && missing == nbBlocks ) /* second part of the array */
        {
            /* we are treating pieces that have not been requested (even partially) yet */
            if( startUnorderedPieces == -1 )
                startUnorderedPieces = (*length);

            endUnorderedPieces = (*length);
        }
        else if( p->requestCount >= missing ) /* third part */
        {
            assert( (startUnorderedPieces == -1 && endUnorderedPieces == -1)
                        || (startUnorderedPieces >= 0 && endUnorderedPieces >= startUnorderedPieces) );
        }
        else if( missing != nbBlocks || p->requestCount > 0 ) /* First part */
        {
            assert( (startUnorderedPieces == -1 && endUnorderedPieces == -1) );
        }
        else  /* there is a problem here */
            assert( FALSE );

        array[*length] = p->index;
        (*length)++;
    }

    /* compute the sum of all the zipf/salt coefficients */
    sum = 0.0;
    for( it=startUnorderedPieces ; it<=endUnorderedPieces ; it++ )
        sum += pieceListLookup( t, array[it] )->salt;

    /* Re-order the normal priority pieces according to the zipf policy */
    while( startUnorderedPieces != endUnorderedPieces )
    {
        float rand;
        float partialSum;
        tr_piece_index_t tmp;

        /* choose a random float in [0 .. sum] */
        rand = ((float) tr_cryptoWeakRandInt( RAND_MAX )) / (float) RAND_MAX * sum;

        partialSum = pieceListLookup( t, array[startUnorderedPieces] )->salt;
        it=startUnorderedPieces;

        /* loop until we find the corresponding piece
         * FIXME: this is really expensive */
        while( rand > partialSum )
            partialSum += pieceListLookup( t, array[++it] )->salt;

        /* put the selected piece at the beginning  */
        tmp = array[it];
        array[it] = array[startUnorderedPieces];
        array[startUnorderedPieces] = tmp;

        /* re-start from the beginning+1 piece */
        sum -= pieceListLookup( t, array[startUnorderedPieces] )->salt;
        startUnorderedPieces++;
    }

    return array;
}




/**
 *    Asserts ( definition )
 */



#ifdef EXTENSIVE_ASSERT_DEBUG

static void
assertRarestPiecesAreSorted( Torrent * t )
{
    if( !isInEndgame( t ) )
    {
        int i;
        weightTorrent = t->tor;
        for( i=0; i<t->pieceCountRarest-1; ++i )
            assert( comparePieceByRareness( &t->piecesRarest[i], &t->piecesRarest[i+1] ) <= 0 );
    }
}

static void
assertOrderPiecesAreSorted( Torrent * t )
{
    if( !isInEndgame( t ) )
    {
        int i;
        weightTorrent = t->tor;
        for( i=0; i<t->pieceCountOrder-1; ++i )
            assert( comparePieceByOrder( &t->piecesOrder[i], &t->piecesOrder[i+1] ) <= 0 );
    }
}

static void
assertWeightedPiecesAreSorted( Torrent * t )
{
    assertRarestPiecesAreSorted( t );
    assertOrderPiecesAreSorted( t );
}

static void
assertReplicationCountIsExact( Torrent * t )
{
    const int pieceCount = t->tor->info.pieceCount;
    const int peerCount = tr_ptrArraySize( &t->peers );
    size_t * replicationCount = tr_new( size_t, pieceCount );
    int itPiece, itPeer; /* iterators */
    tr_peer * peer;

    for( itPiece=0 ; itPiece<pieceCount ; ++itPiece )
    {
        replicationCount[itPiece] = 0;

        for( itPeer=0 ; itPeer<peerCount ; ++itPeer )
        {
            peer = tr_ptrArrayNth( &t->peers, itPeer );
            if( tr_bitsetHasFast( &peer->have , (size_t) itPiece ) )
                ++replicationCount[itPiece];
        }

        assert( t->pieceReplication[itPiece] == replicationCount[itPiece] );

    }
    tr_free( replicationCount );

}

static void
assertWeightedListsAreConsistent( Torrent * t )
{
    int posRar, posOrd;
    tr_piece_index_t i;

    assert( t->pieceCount == (int) t->tor->info.pieceCount );
    assert( t->pieceCountOrder == t->pieceCountRarest );

    for( i=0 ; i<t->tor->info.pieceCount ; i++ )
    {
        posRar = piecePosInRarest( t, i );
        posOrd = piecePosInOrder( t, i );

        assert( ( posRar < 0 && posOrd < 0 )
             || ( posRar >= 0 && posOrd >= 0 ) );
    }
}

#endif
