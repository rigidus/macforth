#include "replication/repl_policy_default.h"

int repl_policy_default_choose(const PolicyCandidate* a, int n, int required_caps){
    int best = -1;
    int best_prio = -2147483647;
    int best_caps = -1;
    for (int i=0;i<n;i++){
        if (!a[i].r) continue;
        if (a[i].health != 0) continue;
        if ((a[i].caps & required_caps) != required_caps) continue;
        if (a[i].priority > best_prio ||
            (a[i].priority == best_prio && a[i].caps > best_caps)){
            best = i; best_prio = a[i].priority; best_caps = a[i].caps;
        }
    }
    return best;
}
