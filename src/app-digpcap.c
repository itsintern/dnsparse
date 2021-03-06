#include "util-pcapfile.h"  /* reads packet capture files */
#include "util-ipdecode.h"  /* decode TCP/IP packets */
#include "util-tcpreasm.h"  /* reassembles TCP streams */
#include "dns-parse.h"      /* decodes DNS payloads */
#include "dns-format.h"     /* prints DNS results */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>


/**
 * Handle a DNS packet, either a UDP packet read from the stream, or a 
 * reassembled TCP payload.
 */
static struct dns_t *
_process_dns(const unsigned char *buf, size_t length, struct dns_t *dns, const char *filename, uint64_t frame_number, int rrtype)
{
    size_t i;
    int err;
    
    /* Decode DNS */
    dns = dns_parse(buf,
                    length,
                    0,
                    dns);
    if (dns == NULL || dns->error_code) {
        fprintf(stderr, "%s:%llu: error parsing DNS\n", filename, frame_number);
        return dns;
    }
    
    /* Process all the records in the DNS packet */
    for (i=0; i<dns->answer_count + dns->nameserver_count + dns->additional_count; i++) {
        const struct dnsrrdata_t *rr = &dns->answers[i];
        char output[65536];

        /* FIXME: remove this */
        if (rr->rtype == DNS_T_OPT)
            continue; /* skip EDNS0 records */
        
        if (rrtype && rr->rtype != rrtype)
            continue;

        /* Format the resource record */
        err = dns_format_rdata(rr, output, sizeof(output));
        if (err)
            continue;

        /* Print in DIG format (i.e. zonefile format) */
        printf("%s%-23s %-7u IN\t%-7s %s\n",
            (rr->section == 0) ? ";" : "",
             rr->name,
             rr->ttl,
             dns_name_from_rrtype(rr->rtype), output);
    }
    
    return dns;
}

/**
 * On TCP, DNS request/responses are prefixed by a two-byte length field
 */
struct dnstcp
{
    int state;
    unsigned short pdu_length;
};

/**
 * Read in the packet-capture file and process all the records.
 */
static void
_process_file(const char *filename, int rrtype)
{
    struct pcapfile_ctx_t *ctx;
    int linktype = 0;
    struct dns_t *recycle = NULL;
    size_t frame_number = 0;
    struct tcpreasm_ctx_t *tcpreasm = 0;
    time_t secs;
    long usecs;
    
    
    /* Open the packet capture file  */
    ctx = pcapfile_openread(filename, &linktype, &secs, &usecs);
    if (ctx == NULL) {
        fprintf(stderr, "[-] error: %s\n", filename);
        return;
    } else {
        time_t now = secs;
        struct tm *tm = gmtime(&now);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(stderr, "[+] %s (%s) %s \n", filename, pcapfile_datalink_name(linktype), timestamp);
    }
    
    /* Create a subsystem for reassembling TCP streams */
    tcpreasm = tcpreasm_create(sizeof(struct dnstcp), 0, secs, 60);
    
    /*
     * Process all the packets read from the file
     */
    for (;;) {
        time_t time_secs;
        long time_usecs;
        size_t original_length;
        size_t captured_length;
        const unsigned char *buf;
        int err;
        struct packetdecode_t decode;
        
        /* Read the next packet */
        err = pcapfile_readframe(ctx, &time_secs, &time_usecs, &original_length, &captured_length, &buf);
        if (err)
            break;
        frame_number++;
        
        /* Decode the packet headers */
        err = util_ipdecode(buf, captured_length, linktype, &decode);
        if (err)
            continue;
        
        /* If not DNS, then ignore this packet */
        if (decode.port_src != 53)
            continue;
        
        if (decode.ip_protocol == 17) {
            /* If UDP, then decode this payload*/
            recycle = _process_dns(buf + decode.app_offset, decode.app_length, recycle, filename, frame_number, rrtype);
        } else if (decode.ip_protocol == 6) {
            /* If TCP, then reassemble the stream into a packet, then
             * decode the reassembled packet if available */
            struct tcpreasm_tuple_t ins;
            
            ins = tcpreasm_packet(tcpreasm, /* reassembler */
                                         buf + decode.ip_offset, /* IP+TCP+payload */
                                         decode.ip_length,
                                         time_secs,             /* timestamp */
                                         time_usecs * 1000);
            if (ins.available) {
                struct dnstcp *d = (struct dnstcp *)ins.userdata;
                if (d->state == 0) {
                    if (ins.available >= 2) {
                        /* First, read the 2-byte header at the start of TCP
                         * to know how long the remaining chunk is going to be */
                        unsigned char foo[2];
                        size_t count;
                        d->state = 1;
                        count = tcpreasm_read(&ins, foo, 2);
                        d->pdu_length = foo[0]<<8 | foo[1];
                        ins.available -= count;
                    }
                }
                if (d->state == 1) {
                    if (d->pdu_length <= ins.available) {
                        /* Once we have enough bytes available to reassemble
                         * the DNS packet, reassemble it and decode it */
                        unsigned char tmp[65536];
                        size_t count;
                        count = tcpreasm_read(&ins, tmp, d->pdu_length);
                        assert(count == d->pdu_length);
                        recycle = _process_dns(tmp, count, recycle, filename, frame_number, rrtype);
                        d->state = 0;
                    }
                }

            }

            /* Process any needed timeouts */
            tcpreasm_timeouts(tcpreasm, time_secs, time_usecs * 1000);
            
        }
    }
    
    /* cleanup allocated memory and exit the function */
    dns_parse_free(recycle);
    pcapfile_close(ctx);
}


int main(int argc, char *argv[])
{
    int i;
    int rrtype = 0;
    
    if (argc <= 1) {
        fprintf(stderr, "[-] no files specified\n");
        return 1;
    }
    
    /* Look for any RRtypes that might be specified */
    for (i=1; i<argc; i++) {
        if (strcmp(argv[i], "-?") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "-- digpcap - extracts DNS records from network packets --\n");
            fprintf(stderr, "usage\n digpcap [rrtype] <filename1> <filename2> ...\n");
            fprintf(stderr, "where:\n rrtype = (optional) A, AAAA, SOA, CNAME, MX, etc.\n filename = pcap/tcpdump file full of packets\n");
            fprintf(stderr, "output:\n same DNS zonefile-compatible output as 'dig'\n");
            exit(0);
        }
        if (dns_rrtype_from_name(argv[i]) > 0) {
            if (rrtype) {
                fprintf(stderr, "[-] fail: only one rrtype can be specified\n");
                exit(1);
            } else
                rrtype = dns_rrtype_from_name(argv[i]);
        }
    }

    /* Process all files listed on the command-line */
    for (i=1; i<argc; i++) {
        _process_file(argv[i], rrtype);
    }
    
    return 0;
}
