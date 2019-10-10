#include "util-tcpreasm.h"
#include "util-hashmap.h"
#include "util-timeout.h"
#include "siphash24.h"
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <assert.h>

uint64_t connection_hash(void *key);

/**
 * A key for randomizeing hashmaps
 */
uint64_t g_hashmap_key[2];

struct tcpreasm_ctx_t {
    Hashmap *conntable;
    size_t sizeof_userdata;
    void (*cleanup_userdata)(void *userdata);
    struct Timeouts *timeouts;
};

struct tcpreasm_connkey_t {
    unsigned char ip_proto;
    unsigned char ip_version;
    unsigned short src_port;
    unsigned short dst_port;
    unsigned char src_ip[16];
    unsigned char dst_ip[16];
};

struct fragment {
    struct fragment *next;
    unsigned seqno;
    unsigned length;
    unsigned char buf[];
};


struct tcpreasm_stream_t {
    struct tcpreasm_connkey_t conn;
    unsigned seqno;
    unsigned is_payload_seen:1;
    struct TimeoutEntry timeout;
    struct fragment *fragments;
    unsigned char userdata[];
};


unsigned SEQNO_LTE(unsigned seqnoA, unsigned seqnoB)
{
    if (seqnoB - seqnoA < 0x80000000)
        return 1;
    else
        return 0;
}
unsigned SEQNO_GT(unsigned seqnoA, unsigned seqnoB)
{
    return !SEQNO_LTE(seqnoA, seqnoB);
}
unsigned SEQNO_GTE(unsigned seqnoA, unsigned seqnoB)
{
    if (seqnoA - seqnoB < 0x80000000)
        return 1;
    else
        return 0;
}
unsigned SEQNO_LT(unsigned seqnoA, unsigned seqnoB)
{
    return !SEQNO_GTE(seqnoA, seqnoB);
}


static int
IS_BETWEEN(unsigned seqno, unsigned begin, unsigned end)
{
    return SEQNO_GTE(seqno, begin) && SEQNO_LTE(seqno, end);
}

/**
 * Merge this fragment and the next fragment if they overflap
 */
static void
_frag_merge(struct fragment **frag, unsigned seqno, const unsigned char *buf, unsigned length)
{
    struct fragment *next_frag = (*frag)->next;
    
    assert(
           IS_BETWEEN(seqno, (*frag)->seqno, (*frag)->seqno + (*frag)->length)
           ||
           IS_BETWEEN(seqno + length, (*frag)->seqno, (*frag)->seqno + (*frag)->length)
           );
    
    if (SEQNO_LT((*frag)->seqno, seqno)) {
        unsigned overlap;
        unsigned diff;
        
        /* Calculate how many bytes of this chunk overlaps
         * with the next chunk, which may be zero, or which
         * may extend past the end of the next packet */
        overlap = (*frag)->seqno + (*frag)->length - seqno;
        
        /* If the existing fragment is completely larger than the overlapping
         * one, then ignore the new input */
        if (overlap >= length)
            return;
        
        /* The amount of new bytes is the new fragment minus what already exists */
        diff = length - overlap;
        
        /* Grow the existing fragment */
        (*frag) = realloc(*frag, sizeof(**frag) + (*frag)->length + diff);
        
        if (*frag == NULL) {
            /* out-of-memory. In this case, we'll remove all the fragments
             * that we have, starting from the "next" pointer that we
             * remembered from above */
            while (next_frag) {
                *frag = next_frag;
                next_frag = next_frag->next;
                free(*frag);
            }
            return;
        }
        
        memcpy((*frag)->buf + (*frag)->length, buf + overlap, diff);
        (*frag)->length += diff;
    } else {
        unsigned overlap;
        unsigned diff;
        
        /* Calculate how many bytes of this chunk overlaps
         * with the next chunk, which may be zero, or which
         * may extend past the end of the next packet */
        overlap = seqno + length - (*frag)->seqno;
        
        /* If the NEW fragment completely overlaps the OLD one, then
         * replace the old one with the new one */
        if (overlap >= (*frag)->length) {
            (*frag) = realloc(*frag, sizeof(**frag) + length);
            
            if (*frag == NULL) {
                /* Out-of-Memory: delete all fragments */
                while (next_frag) {
                    *frag = next_frag;
                    next_frag = next_frag->next;
                    free(*frag);
                }
                return;
            }
            
            memcpy((*frag)->buf, buf, length);
            (*frag)->length = length;
            (*frag)->seqno = seqno;
            return;
        }
        
        /* The amount of new bytes is the new fragment minus what already exists */
        diff = (*frag)->length - overlap;
        
        /* Grow the existing fragment */
        (*frag) = realloc(*frag, sizeof(**frag) + (*frag)->length + diff);
        
        if (*frag == NULL) {
            /* Out-of-Memory: delete all fragments */
            while (next_frag) {
                *frag = next_frag;
                next_frag = next_frag->next;
                free(*frag);
            }
            return;
        }
        
        memmove((*frag)->buf + length, (*frag)->buf + overlap, diff);
        memcpy((*frag)->buf, buf, length);
        (*frag)->length = length + diff;
        (*frag)->seqno = seqno;
    }
}

struct fragment *
_fragment_new(unsigned seqno, const unsigned char *buf, unsigned length, struct fragment *next)
{
    struct fragment *newfrag;
    
    newfrag = malloc(sizeof(*newfrag) + length);
    
    /* In case of memory allocation error, just return the
     * next fragment -- as if there was no creation */
    if (newfrag == NULL)
        return next;
    
    newfrag->seqno = seqno;
    newfrag->length = length;
    memcpy(newfrag->buf, buf, length);
    newfrag->next = next;
    return newfrag;
}

static int
_frag_is_overlap(struct fragment *frag, unsigned seqno, unsigned length)
{
    if (SEQNO_LT(seqno + length + 1, frag->seqno))
        return 0;
    else if (SEQNO_GT(seqno, frag->seqno + frag->length + 1))
        return 0;
    else
        return 1;
}
static int
_frag_is_after(struct fragment *frag, unsigned seqno, unsigned length)
{
    return SEQNO_GT(frag->seqno, seqno + length + 1);
}
static size_t
tcp_append(struct tcpreasm_stream_t *stream, unsigned seqno, const unsigned char *buf, unsigned length)
{
    /* If there's nothing here, such as an ACK packet, then
     * return immediately */
    if (length == 0)
        goto end;
    
    /* set the first sequence number */
    if (!stream->is_payload_seen) {
        stream->seqno = seqno;
        stream->is_payload_seen = 1;
    }
    
    if (stream->fragments == NULL) {
        stream->fragments = _fragment_new(seqno, buf, length, NULL);
    } else {
        struct fragment **frag;
        
        for (frag=&stream->fragments; *frag; frag = &(*frag)->next) {
            if (_frag_is_overlap(*frag, seqno, length)) {
                _frag_merge(frag, seqno, buf, length);
                if ((*frag)->next && _frag_is_overlap(*frag, (*frag)->next->seqno, (*frag)->next->length)) {
                    printf("unhandled condition: double overlap\n");
                    /* FIXME: what if the incoming fragment is so big it overlaps multiple existing fragments? */
                }
                length = 0;
            } else if (_frag_is_after(*frag, seqno, length))
                break;
        }
        if (length)
            *frag = _fragment_new(seqno, buf, length, *frag);
    }
    
    
    
end:
    if (stream->fragments) {
        printf("seqno=%u fragno=%u length=%u\n", stream->seqno, stream->fragments->seqno, stream->fragments->length);
    }
    if (stream->fragments && stream->fragments->seqno == stream->seqno) {
        return stream->fragments->length;
    } else if (stream->fragments) {
        return 0;
    } else {
        return 0;
    }
}


uint64_t connection_hash(void *key)
{
    return siphash24(key, sizeof(struct tcpreasm_connkey_t), g_hashmap_key);
}

bool connection_compare(void* keyA, void* keyB)
{
    return memcmp(keyA, keyB, sizeof(struct tcpreasm_connkey_t)) == 0;
}

struct tcpreasm_ctx_t *
tcpreasm_create(size_t userdata_size, void (*cleanup)(void *userdata), unsigned secs, unsigned usecs)
{
    struct tcpreasm_ctx_t *ctx;
    
    /* Create a hashmap key to make hashtables unpredictable.
     * FIXME: this should grab something more random */
    if (g_hashmap_key[0] == 0) {
        struct timeval tv;
        gettimeofday(&tv, 0);
        g_hashmap_key[0] = tv.tv_sec * 1000000000ULL + tv.tv_usec;
        gettimeofday(&tv, 0);
        g_hashmap_key[1] = tv.tv_sec * 1000000000ULL + tv.tv_usec;
    }
    
    /* Allocate the object and set to zero */
    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL)
        return NULL;
    
    /* Create the hashmap for TCP connections */
    ctx->conntable = hashmapCreate(1024, connection_hash, connection_compare);
    ctx->sizeof_userdata = userdata_size;
    ctx->cleanup_userdata = cleanup;

    ctx->timeouts = timeouts_create(secs, usecs);
    return ctx;
}

struct tcpreasm_stream_t *
_stream_new(struct tcpreasm_ctx_t *ctx, const struct tcpreasm_connkey_t *conn, unsigned secs, unsigned usecs)
{
    struct tcpreasm_stream_t *stream;
    
    stream = calloc(1, sizeof(*stream) + ctx->sizeof_userdata);
    if (stream == NULL)
        return NULL;
    memcpy(&stream->conn, conn, sizeof(*conn));

    timeouts_add(ctx->timeouts, &stream->timeout, offsetof(struct tcpreasm_stream_t, timeout), timestamp_from_tv(secs + 100, usecs));

    hashmapPut(ctx->conntable, &stream->conn, stream);
    
    return stream;
}

static void
_stream_delete(struct tcpreasm_ctx_t *ctx, const struct tcpreasm_stream_t *in_stream)
{
    struct tcpreasm_stream_t *stream;
    
    stream = hashmapRemove(ctx->conntable, (void**)&in_stream->conn);
    assert(stream != NULL);
    
    /* Remove from the timeouts structure */
    timeout_unlink(&stream->timeout);
    
    while (stream->fragments) {
        struct fragment *frag;
        frag = stream->fragments;
        stream->fragments = frag->next;
        free(frag);
    }
    
    if (ctx->cleanup_userdata)
        ctx->cleanup_userdata(stream->userdata);
    free(stream);
}

struct tcpreasm_handle_t
tcpreasm_insert_packet(struct tcpreasm_ctx_t *ctx, const unsigned char *buf, size_t length, unsigned secs, unsigned usecs)
{
    struct tcpreasm_handle_t result = {0};
    struct tcpreasm_connkey_t conn = {0};
    size_t offset = 0;
    size_t hdrlen;
    struct tcpreasm_stream_t *stream;
    unsigned seqno;
    size_t payload_length;
    unsigned tcp_flags;
    enum {
        FIN = 1,
        SYN = 2,
        RST = 4
    };
    
    /* Get the IP version */
    if (length < 1)
        goto fail;
    conn.ip_version = buf[0]>>4;
    
    /* Decode the IP header*/
    switch (conn.ip_version) {
        default:
            goto fail;
        case 4:
            if (length < 20)
                goto fail;
            hdrlen = (buf[0]&0xf) * 4;
            if (length < hdrlen)
                goto fail;
            conn.ip_proto = buf[9];
            memcpy(conn.src_ip, buf + 12, 4);
            memcpy(conn.dst_ip, buf + 16, 4);
            offset += hdrlen;
            break;
        case 6:
            if (length < 40)
                goto fail;
            hdrlen = 40;
            conn.ip_proto = buf[6];
            memcpy(conn.src_ip, buf + 8, 16);
            memcpy(conn.dst_ip, buf+ 24, 16);
            offset += hdrlen;
            
            while (conn.ip_proto != 6) {
                unsigned char ext_len;
                if (conn.ip_proto == 1)
                    goto fail; /* ICMP */
                if (conn.ip_proto == 17)
                    goto fail; /* UDP */
                if (conn.ip_proto == 132)
                    goto fail; /* SCTP */
                if (offset + 8 > length)
                    goto fail;
                ext_len = buf[offset+1];
                if (8 + ext_len > length)
                    goto fail;
                conn.ip_proto = buf[offset];
                offset += 8 + ext_len;
            }
            break;
    }
    
    /* Decode the TCP portion */
    if (offset + 20 > length)
        goto fail;
    hdrlen = (buf[offset + 12]>>4) * 4;
    if (offset + hdrlen > length)
        goto fail;
    tcp_flags = buf[offset + 13];
    
    conn.src_port = buf[offset + 0] << 8 | buf[offset + 1];
    conn.dst_port = buf[offset + 2] << 8 | buf[offset + 3];
    seqno = buf[offset + 4]<<24 | buf[offset + 5] << 16 | buf[offset + 6] << 8 | buf[offset + 7];
    offset += hdrlen;
    payload_length = length - offset;
    
    /* Now lookup the entry in the hash table */
    stream = hashmapGet(ctx->conntable, &conn);
    if (stream == NULL) {
        
        if ((tcp_flags & SYN) == 0) {
            /* this is some extraneous fragment */
            return result;
        }
        
        stream = _stream_new(ctx, &conn, secs, usecs);
        
        if (stream == NULL) {
            /* out of memory */
            return result;
        }
        
        /* initial sequence number for data, SYN=1-byte of virtual data */
        stream->seqno = seqno + 1;
        
    } else if ((tcp_flags & RST) != 0) {
        /* A RST was received, indicating the connection needs to be
         * closed */
        _stream_delete(ctx, stream);
    } else if ((tcp_flags & FIN) != 0) {
        /* The connection was closed, ony close the connection if
         * there's no pending data, otherwise, wait for timeout
         * to cleanup the connection */
        if (stream->fragments == NULL)
            _stream_delete(ctx, stream);
    }

    /* Append this data */
    result.conn = &stream->conn;
    result.available = tcp_append(stream, seqno, buf + offset, (unsigned)payload_length);
    result.userdata = &stream->userdata;
    result.ctx = ctx;
    return result;
    
fail:
    result.available = 0;
    return result;
}


size_t tcpreasm_read(struct tcpreasm_handle_t *handle, unsigned char *buf, size_t length)
{
    struct tcpreasm_stream_t *stream;
    struct fragment *frag;
    
    /* Get a handel to the stream */
    stream = hashmapGet(handle->ctx->conntable, handle->conn);
    if (stream == NULL)
        return 0;
    
    /* Make sure the there's any data avaialble */
    frag = stream->fragments;
    if (frag == NULL)
        return 0;
    if (frag->seqno != stream->seqno)
        return 0;
    
    /* If asking for more data than exists, shrink to how much is available */
    if (length > frag->length)
        length = frag->length;
    
    /* Copy over the number of bytes */
    memcpy(buf, frag->buf, length);
    
    /* Shrink the curent fragment */
    if (length >= frag->length) {
        assert(length == frag->length);
        stream->fragments = frag->next;
        free(frag);
    } else {
        frag->length -= length;
        memmove(frag->buf, frag->buf + length, frag->length - length);
        frag->seqno += length;
    }
    stream->seqno += length;
    
    return length;
}