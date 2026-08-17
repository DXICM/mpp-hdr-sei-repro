/* Probe per-frame HDR dynamic metadata export of MPP's h265 decoder.
 * Feeds the stream AU-by-AU (like FFmpeg's rkmppdec, which gets complete
 * access units from the demuxer) and prints for every decoded frame whether
 * mpp_frame_get_hdr_dynamic_meta() returns data.
 *   -f : enable MPP_DEC_SET_PARSER_FAST_MODE at info change, mirroring
 *        rkmppdec's default (fast_parse=1).
 * Build: gcc hdr_probe.c -o hdr_probe -lrockchip_mpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

static int n_frames, n_meta;
static int fast_mode;

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f"))
            fast_mode = 1;
        else
            file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "usage: %s [-f] file.h265\n", argv[0]);
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

    /* split into AUs at each VPS (all-intra streams) */
    long offs[256];
    int n_au = 0;
    for (long i = 0; i + 5 < sz && n_au < 256; i++) {
        int sc3 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1);
        int sc4 = (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1);
        if (!sc3 && !sc4)
            continue;
        if (i > 0 && buf[i-1] == 0 && sc3)
            continue;
        long p = i + (sc3 ? 3 : 4);
        if (((buf[p] >> 1) & 0x3F) == 32)
            offs[n_au++] = i;
    }
    printf("%s: %d AUs, fast_mode=%d\n", file, n_au, fast_mode);

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    if (mpp_create(&ctx, &mpi) || mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC)) {
        fprintf(stderr, "mpp init failed\n");
        return 1;
    }

    char tag[32];
    for (int a = 0; a <= n_au; a++) {
        MppPacket pkt = NULL;
        if (a < n_au) {
            long start = offs[a];
            long end = (a + 1 < n_au) ? offs[a + 1] : sz;
            mpp_packet_init(&pkt, buf + start, end - start);
            mpp_packet_set_pts(pkt, a);
        } else {
            mpp_packet_init(&pkt, buf, 0);
            mpp_packet_set_eos(pkt);
        }
        snprintf(tag, sizeof(tag), "AU%d", a);
        int tries = 0;
        while (mpi->decode_put_packet(ctx, pkt) != MPP_OK && tries++ < 2000) {
            drain(ctx, mpi, tag);
            usleep(1000);
        }
        mpp_packet_deinit(&pkt);
        drain(ctx, mpi, tag);
    }

    int eos_seen = 0;
    for (int i = 0; i < 1000 && !eos_seen; i++) {
        MppFrame frame = NULL;
        if (mpi->decode_get_frame(ctx, &frame) || !frame) {
            usleep(2000);
            continue;
        }
        if (mpp_frame_get_eos(frame)) {
            eos_seen = 1;
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

    printf("SUMMARY: frames=%d with_dynamic_meta=%d eos=%d\n", n_frames, n_meta, eos_seen);
    mpp_destroy(ctx);
    free(buf);
    return 0;
}
