/* Probe per-frame HDR dynamic metadata export of MPP's h265 decoder.
 * Modes:
 *   default : feed complete AUs one by one (FFmpeg rkmppdec style)
 *   -c      : feed in 8 KB chunks (mpi_dec_test style)
 *   -w      : wait/poll for frames after each packet (drain as they arrive)
 *   -f      : enable MPP_DEC_SET_PARSER_FAST_MODE at info change
 *             (rkmppdec default fast_parse=1)
 * AU boundaries: AUD NALs (type 35) when present, else VPS (type 32).
 * Build: gcc hdr_probe.c -o hdr_probe -lrockchip_mpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

#define CHUNK 8192

static int n_frames, n_meta;
static int fast_mode, chunk_mode, wait_mode;

static void report(MppFrame frame, const char *tag)
{
    n_frames++;
    RK_S32 poc = mpp_frame_get_poc(frame);
    RK_U32 err = mpp_frame_get_errinfo(frame);
    RK_U32 disc = mpp_frame_get_discard(frame);
    MppFrameHdrDynamicMeta *m = mpp_frame_get_hdr_dynamic_meta(frame);
    if (m && m->size >= 8) {
        n_meta++;
        printf("frame #%d poc=%2d meta=PRESENT fmt=%u size=%u head=%02x%02x%02x%02x%02x%02x%02x%02x%s%s [%s]\n",
               n_frames, poc, m->hdr_fmt, m->size,
               m->data[0], m->data[1], m->data[2], m->data[3],
               m->data[4], m->data[5], m->data[6], m->data[7],
               err ? " ERR" : "", disc ? " DISCARD" : "", tag);
    } else {
        printf("frame #%d poc=%2d meta=absent%s%s [%s]\n",
               n_frames, poc, err ? " ERR" : "", disc ? " DISCARD" : "", tag);
    }
}

static void drain(MppCtx ctx, MppApi *mpi, const char *tag)
{
    while (1) {
        MppFrame frame = NULL;
        if (mpi->decode_get_frame(ctx, &frame) || !frame)
            break;
        if (mpp_frame_get_eos(frame)) {
            mpp_frame_deinit(&frame);
            break;
        }
        if (mpp_frame_get_info_change(frame)) {
            printf("  [info change %ux%u]\n",
                   mpp_frame_get_width(frame), mpp_frame_get_height(frame));
            if (fast_mode) {
                RK_U32 on = 1;
                mpi->control(ctx, MPP_DEC_SET_PARSER_FAST_MODE, &on);
                printf("  [fast parse mode enabled]\n");
            }
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&frame);
            continue;
        }
        report(frame, tag);
        mpp_frame_deinit(&frame);
    }
}

static void flush(MppCtx ctx, MppApi *mpi)
{
    for (int i = 0; i < 2000; i++) {
        MppFrame frame = NULL;
        if (mpi->decode_get_frame(ctx, &frame) || !frame) {
            usleep(2000);
            continue;
        }
        if (mpp_frame_get_eos(frame)) {
            mpp_frame_deinit(&frame);
            break;
        }
        if (mpp_frame_get_info_change(frame)) {
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&frame);
            continue;
        }
        report(frame, "flush");
        mpp_frame_deinit(&frame);
    }
}

/* returns number of AU start offsets found */
static int split_aus(const unsigned char *buf, long sz, long *offs, int max)
{
    int has_aud = 0;
    int n = 0;
    for (long i = 0; i + 5 < sz; i++) {
        int sc3 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1);
        int sc4 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1);
        if (!sc3 && !sc4)
            continue;
        if (i > 0 && buf[i-1] == 0 && sc3)
            continue;
        long p = i + (sc3 ? 3 : 4);
        if (((buf[p] >> 1) & 0x3F) == 35) {
            has_aud = 1;
            break;
        }
    }
    for (long i = 0; i + 5 < sz && n < max; i++) {
        int sc3 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1);
        int sc4 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1);
        if (!sc3 && !sc4)
            continue;
        if (i > 0 && buf[i-1] == 0 && sc3)
            continue;
        long p = i + (sc3 ? 3 : 4);
        RK_U32 t = (buf[p] >> 1) & 0x3F;
        if ((has_aud && t == 35) || (!has_aud && t == 32))
            offs[n++] = i;
    }
    return n;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f"))
            fast_mode = 1;
        else if (!strcmp(argv[i], "-c"))
            chunk_mode = 1;
        else if (!strcmp(argv[i], "-w"))
            wait_mode = 1;
        else
            file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "usage: %s [-f] [-c] file.h265\n", argv[0]);
        return 1;
    }
    FILE *fp = fopen(file, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(fp);

    static long offs[4096];
    int n_au = chunk_mode ? 0 : split_aus(buf, sz, offs, 4096);
    printf("%s: mode=%s fast=%d AUs=%d\n", file, chunk_mode ? "chunk" : "au",
           fast_mode, n_au);

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    if (mpp_create(&ctx, &mpi) || mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC)) {
        fprintf(stderr, "mpp init failed\n");
        return 1;
    }
    if (chunk_mode) {
        RK_U32 split = 1;
        mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    }

    char tag[32];
    int n_pkts = chunk_mode ? (int)((sz + CHUNK - 1) / CHUNK) : n_au;
    for (int a = 0; a <= n_pkts; a++) {
        MppPacket pkt = NULL;
        if (a < n_pkts) {
            long start, end;
            if (chunk_mode) {
                start = (long)a * CHUNK;
                end = start + CHUNK < sz ? start + CHUNK : sz;
            } else {
                start = offs[a];
                end = (a + 1 < n_au) ? offs[a + 1] : sz;
            }
            mpp_packet_init(&pkt, buf + start, end - start);
            mpp_packet_set_pts(pkt, a);
        } else {
            mpp_packet_init(&pkt, buf, 0);
            mpp_packet_set_eos(pkt);
        }
        snprintf(tag, sizeof(tag), chunk_mode ? "C%d" : "AU%d", a);
        int tries = 0;
        while (mpi->decode_put_packet(ctx, pkt) != MPP_OK && tries++ < 5000) {
            drain(ctx, mpi, tag);
            usleep(1000);
        }
        mpp_packet_deinit(&pkt);
        drain(ctx, mpi, tag);
        if (wait_mode) {
            for (int w = 0; w < 50; w++) {
                usleep(2000);
                drain(ctx, mpi, tag);
            }
        }
    }

    flush(ctx, mpi);
    printf("SUMMARY: frames=%d with_dynamic_meta=%d\n", n_frames, n_meta);
    mpp_destroy(ctx);
    free(buf);
    return 0;
}
