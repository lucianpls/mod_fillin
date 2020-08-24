/*
* mod_reproject.h
*
*/

#if !defined(MOD_AHTSE_FILL_H)
#define MOD_AHTSE_FILL_H

#include <httpd.h>
#include <http_config.h>

struct ahtse_fill_conf {
    // http_root path of this configuration
    const char *doc_path;

    // array of guard regexp, one of them has to match
    apr_array_header_t *arxp;
};

#endif
