# -*- coding: utf-8 -*-
import io, sys
p = r"D:\Repo\LinuxTestDrive\reposync\src\index.c"
s = io.open(p, encoding='utf-8').read()

old = """    str_list uris, suites, comps, arches;
    sl_init(&uris); sl_init(&suites); sl_init(&comps); sl_init(&arches);
    d822_get_list(r, "uris", &uris);
    d822_get_list(r, "suites", &suites);"""

new = """    str_list uris, suites, comps, arches;
    sl_init(&uris); sl_init(&suites); sl_init(&comps); sl_init(&arches);
    d822_get_list(r, "uris", &uris);

    /* HTTP(S) sources are preferred over file: when a source lists several
       URIs, the http(s) ones are tried first (same behaviour as apt with
       multiple URIs). fetch() is libcurl either way. */
    {
        size_t w = 0, ui;
        for (ui = 0; ui < uris.n; ui++) {
            if (strncmp(uris.items[ui], "http://", 7) == 0 ||
                strncmp(uris.items[ui], "https://", 8) == 0) {
                char *t = uris.items[w];
                uris.items[w] = uris.items[ui];
                uris.items[ui] = t;
                w++;
            }
        }
    }
    d822_get_list(r, "suites", &suites);"""

if old not in s:
    print("NOT FOUND")
    sys.exit(1)
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print("http-first sort patched OK")
