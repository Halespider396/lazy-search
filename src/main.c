#include <stdio.h>
#include "package.h"
#include "pacman_source.h"
#include "aur_source.h"
#include "tui.h"

#define AUR_CACHE_MAX_AGE (24L * 60 * 60) /* refresh AUR list once a day */

int main(void) {
    PackageList list;
    pkglist_init(&list);

    fprintf(stderr, "lazy-search: loading official repo packages...\n");
    if (pacman_load_packages(&list) != 0) {
        fprintf(stderr, "lazy-search: warning: couldn't run pacman -Sl\n");
    }

    fprintf(stderr, "lazy-search: preloading official-repo descriptions...\n");
    pacman_bulk_load_descriptions(&list);

    fprintf(stderr, "lazy-search: loading AUR package list (cached daily)...\n");
    if (aur_load_packages(&list, AUR_CACHE_MAX_AGE) != 0) {
        fprintf(stderr, "lazy-search: warning: no AUR data available (offline and no cache yet)\n");
    }

    if (list.count == 0) {
        fprintf(stderr, "lazy-search: no packages loaded, nothing to search.\n");
        pkglist_free(&list);
        return 1;
    }

    char selection[NAME_MAX_LEN + REPO_MAX_LEN + 2] = {0};
    int status = tui_run(&list, selection, sizeof(selection));

    pkglist_free(&list);

    if (status != 0) {
        return 1; /* cancelled */
    }

    /* Prints only the selection ("repo/name") to stdout, so it can be
     * captured in a wrapper script and piped into pacman/yay -S. */
    printf("%s\n", selection);
    return 0;
}
