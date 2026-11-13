/* copilot.c - point 5: an advisory copilot with a learned decision tree. QNX process, LOW priority (8).
 *
 * It never touches the ventilator. Each breath:
 *   1. MsgSend(COPILOT_GET) to safety_mon. safety_mon keeps the reply until a breath closes (deferred reply), so the
 *      copilot sleeps, blocked in the kernel, without polling.
 *   2. The reply is the breath as safety_mon measured it (cp_breath_t: features, settings, Vt, auto-PEEP).
 *   3. tree_predict() walks the tree learned offline (src/copilot_tree.h; the walk is the user's task 5) -> class.
 *   4. cp_rules(): the FIXED table turns the class into an action (copilot_logic.c).
 *   5. MsgSend(COPILOT_SUGGEST). safety_mon checks it against its own measurements; the operator decides.
 * safety_mon launches it with the derived security type "copilot", which may only connect to safety_mon (not the
 * plant, not ventd), and relaunches it if it dies. If the copilot hangs or dies, ventilation does not notice.
 *
 * Use: copilot [-p prio] [-s secs] [-o breaths.csv]
 * Build with -DCOPILOT_LIAR for the test-only liar (in /system/bg like evil_hmi, but the boot never starts it): it
 * claims "low compliance" on every breath, proposes Pinsp + 6, tries to accept its own proposal (directly and
 * through /dev/ventilator/copilot) and tries to open the plant. */
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include <unistd.h>

#include "bg_proto.h"
#include "copilot_logic.h"
#include "copilot_tree.h"
#include "rt_util.h"
#include "tree_predict.h"

#ifdef COPILOT_LIAR
#define PROG "liar_copilot"
#else
#define PROG "copilot"
#endif

static volatile sig_atomic_t stop;
static void on_term(int sig) { (void)sig; stop = 1; }

int main(int argc, char **argv)
{
    int prio = 8, opt;
    double secs = 0;
    const char *out_path = NULL;
    while ((opt = getopt(argc, argv, "p:s:o:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'o': out_path = optarg; break;
        default: fprintf(stderr, "use: %s [-p prio] [-s secs] [-o breaths.csv]\n", PROG); return 1;
        }
    }
    secs = rt_duration(secs);
    signal(SIGTERM, on_term);

    int safety = name_open(BG_SAFETY_NAME, 0);
    if (safety == -1) rt_die(PROG, "name_open(" BG_SAFETY_NAME ")");
    rt_setup(PROG, prio);
    FILE *out = NULL;
    if (out_path) {
        out = fopen(out_path, "a"); /* append: a relaunched copilot keeps what the previous one wrote */
        if (!out) rt_die(PROG, out_path);
        fseek(out, 0, SEEK_END); /* in append mode ftell() may say 0 until the first write */
        if (ftell(out) == 0) {
            fprintf(out, "t_s,breath,valid,cls,conf,action,infer_ns,vti_ml,vte_ml,auto_peep");
            for (int k = 0; k < BF_N; k++) fprintf(out, ",%s", bf_names[k]);
            fprintf(out, "\n");
        }
    }

    const tp_tree_t tree = { .n_nodes = CT_N_NODES, .n_features = CT_N_FEATURES, .n_classes = CT_N_CLASSES,
                             .feature = ct_feature, .threshold = ct_threshold, .left = ct_left, .right = ct_right,
                             .value = &ct_value[0][0] };
    const double inv_ns = 1e9 / (double)SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    double t0 = rt_now_s();
    uint32_t after = 0, n = 0, n_valid = 0, n_class[CT_N_CLASSES] = { 0 }, ns_max = 0;
    printf("%s: prio %d, tree of %d nodes (depth %d)\n", PROG, prio, CT_N_NODES, CT_DEPTH);
    fflush(stdout);

    while (!stop && rt_now_s() - t0 < secs) {
        bg_copilot_get_t q = { .hdr = { .type = BG_MSG_COPILOT_GET }, .after = after };
        cp_breath_t b;
        if (MsgSend(safety, &q, sizeof q, &b, sizeof b) == -1) {
            if (errno == EINTR) continue;
            rt_die(PROG, "MsgSend(COPILOT_GET)"); /* safety_mon is gone */
        }
        after = b.breath;
        n++;
        cp_suggest_t s;
        uint64_t c0 = ClockCycles();
        int cls = -1;
        double conf = 0;
        if (b.valid) {
            cls = tree_predict(&tree, b.f, &conf);
            n_valid++;
        }
        cp_rules(&b, cls, conf, &s);
        uint32_t ns = (uint32_t)((double)(ClockCycles() - c0) * inv_ns);
        s.infer_ns = ns;
        if (ns > ns_max) ns_max = ns;
        if (cls >= 0 && cls < CT_N_CLASSES) n_class[cls]++;
#ifdef COPILOT_LIAR
        s.cls = CP_LOW_C;
        s.conf = 0.99f;
        s.action = CP_ACT_RAISE_PINSP;
        s.req = b.set;
        s.req.set.p_insp = b.set.set.p_insp + 6;
        for (uint32_t id = 1; id <= 3; id++) { /* accept "its own" proposal: must be refused (EPERM) */
            bg_copilot_ack_t ack = { .hdr = { .type = BG_MSG_COPILOT_ACK }, .id = id, .accept = 1 };
            bg_copilot_ack_reply_t r;
            if (MsgSend(safety, &ack, sizeof ack, &r, sizeof r) == -1 && n == 1)
                printf("%s: ACK %u refused: %s\n", PROG, id, strerror(errno));
        }
        if (n == 1) {
            int p = name_open(BG_PNEUMATIC_NAME, 0);
            printf("%s: name_open(plant) = %d (%s)\n", PROG, p, p == -1 ? strerror(errno) : "OPENED");
            if (p != -1) name_close(p);
            int fd = open("/dev/ventilator/copilot", O_WRONLY); /* the operator's door */
            printf("%s: open(/dev/ventilator/copilot) = %d (%s)\n", PROG, fd, fd == -1 ? strerror(errno) : "OPENED");
            if (fd != -1) {
                ssize_t w = write(fd, "accept 1\n", 9);
                printf("%s: write(accept 1) = %zd (%s)\n", PROG, w, w == -1 ? strerror(errno) : "written");
                close(fd);
            }
            fflush(stdout);
        }
#endif
        bg_copilot_suggest_t m = { .hdr = { .type = BG_MSG_COPILOT_SUGGEST }, .s = s };
        if (MsgSend(safety, &m, sizeof m, NULL, 0) == -1 && errno != EINTR && errno != EINVAL && errno != EPERM)
            rt_die(PROG, "MsgSend(COPILOT_SUGGEST)");
        if (out) {
            fprintf(out, "%.6f,%u,%d,%d,%.4f,%d,%u,%.1f,%.1f,%.2f", rt_now_s(), b.breath, b.valid, s.cls, s.conf,
                    s.action, ns, b.vti_ml, b.vte_ml, b.auto_peep);
            for (int k = 0; k < BF_N; k++) fprintf(out, ",%.6g", b.f[k]);
            fprintf(out, "\n");
            fflush(out); /* a killed copilot keeps what it wrote */
        }
    }
    rt_relax();
    printf("%s: %u breaths (%u valid): normal %u, leak %u, high_r %u, low_c %u; tree+rules max %.1f us\n", PROG, n,
           n_valid, n_class[0], n_class[1], n_class[2], n_class[3], ns_max / 1e3);
    if (out) fclose(out);
    return 0;
}
