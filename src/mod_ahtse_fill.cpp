/*
* mod_ahtse_fill.cpp
* An AHTSE module that fills in the missing gaps in a tile pyramid, using data from the 
* lower resolution levels, if those are available
*
* (C) Lucian Plesea 2017
*/

// TODO: Everything

#include "mod_ahtse_fill.h"
// #include <apr_strings.h>

static void *create_dir_conf(apr_pool_t *p, char *path)
{
    ahtse_fill_conf *c = (ahtse_fill_conf *) apr_pcalloc(p, sizeof(ahtse_fill_conf));
    c->doc_path = path;
    return c;
}

// Allow for one or more RegExp guard
// One of them has to match if the request is to be considered
static const char *set_regexp(cmd_parms *cmd, ahtse_fill_conf *c, const char *pattern)
{
    char *err_message = NULL;
    if (c->arxp == 0)
        c->arxp = apr_array_make(cmd->pool, 2, sizeof(ap_regex_t *));
    ap_regex_t **m = (ap_regex_t **)apr_array_push(c->arxp);
    *m = ap_pregcomp(cmd->pool, pattern, 0);
    return (NULL != *m) ? NULL : "Bad regular expression";
}

static const char *read_config(cmd_parms *cmd, ahtse_fill_conf *c, const char *src, const char *fname)
{
    return "UNIMPLEMENTED";
}

static void register_hooks(apr_pool_t *p) {
//    ap_hook_handler(handler, NULL, NULL, APR_HOOK_MIDDLE);
}

static const command_rec cmds[] =
{
    AP_INIT_TAKE2(
        "Fill_ConfigurationFiles",
        (cmd_func)read_config, // Callback
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Source and output configuration files"
    ),

    AP_INIT_TAKE1(
        "Fill_RegExp",
        (cmd_func)set_regexp,
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Regular expression that the URL has to match.  At least one is required."
    ),

    { NULL }
};

module AP_MODULE_DECLARE_DATA ahtse_fill_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_conf,
    0, // No dir_merge
    0, // No server_config
    0, // No server_merge
    cmds, // configuration directives
    register_hooks // processing hooks
};
