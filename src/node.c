#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Jemalloc memory management
#include <jemalloc/jemalloc.h>

// PCRE
#include <pcre.h>

// Judy array
#include <Judy.h>

#include "define.h"
#include "str.h"
#include "node.h"
#include "token.h"


// String value as the index http://judy.sourceforge.net/doc/JudySL_3x.htm

/**
 * Create a rnode object
 */
rnode * rnode_create(int cap) {
    rnode * n = (rnode*) malloc( sizeof(rnode) );

    n->edges = (redge**) malloc( sizeof(redge*) * 10 );
    n->edge_len = 0;
    n->edge_cap = 10;
    n->endpoint = 0;
    n->combined_pattern = NULL;
    // n->edge_patterns = token_array_create(10);
    return n;
}

void rnode_free(rnode * tree) {
    for (int i = 0 ; i < tree->edge_len ; i++ ) {
        if (tree->edges[i]) {
            redge_free(tree->edges[ i ]);
        }
    }
    free(tree->edges);
    // token_array_free(tree->edge_patterns);
    free(tree);
    tree = NULL;
}



/* parent node, edge pattern, child */
redge * rnode_add_child(rnode * n, char * pat , rnode *child) {
    // find the same sub-pattern, if it does not exist, create one

    redge * e;

    e = rnode_find_edge(n, pat);
    if (e) {
        return e;
    }

    e = redge_create( pat, strlen(pat), child);
    rnode_append_edge(n, e);
    // token_array_append(n->edge_patterns, pat);
    // assert( token_array_len(n->edge_patterns) == n->edge_len );
    return e;
}



void rnode_append_edge(rnode *n, redge *e) {
    if (n->edge_len >= n->edge_cap) {
        n->edge_cap *= 2;
        n->edges = realloc(n->edges, sizeof(redge) * n->edge_cap);
    }
    n->edges[ n->edge_len++ ] = e;
}


redge * rnode_find_edge(rnode * n, char * pat) {
    redge * e;
    for (int i = 0 ; i < n->edge_len ; i++ ) {
        e = n->edges[i];
        if ( strcmp(e->pattern, pat) == 0 ) {
            return e;
        }
    }
    return NULL;
}

void rnode_compile(rnode *n)
{
    bool use_slug = rnode_has_slug_edges(n);
    if ( use_slug ) {
        rnode_compile_patterns(n);
    } else {
        // use normal text matching...
        n->combined_pattern = NULL;
    }

    for (int i = 0 ; i < n->edge_len ; i++ ) {
        rnode_compile(n->edges[i]->child);
    }
}


/**
 * This function combines ['/foo', '/bar', '/{slug}'] into (/foo)|(/bar)|/([^/]+)}
 *
 */
void rnode_compile_patterns(rnode * n) {
    char * cpat;
    char * p;

    cpat = calloc(sizeof(char),128);
    if (cpat==NULL)
        return;

    p = cpat;

    redge *e = NULL;
    for ( int i = 0 ; i < n->edge_len ; i++ ) {
        e = n->edges[i];
        if ( e->has_slug ) {
            char * slug_pat = compile_slug(e->pattern, e->pattern_len);
            strcat(p, slug_pat);
        } else {
            strncat(p++,"(", 1);

            strncat(p, e->pattern, e->pattern_len);
            p += e->pattern_len;

            strncat(p++,")", 1);
        }

        if ( i + 1 < n->edge_len ) {
            strncat(p++,"|",1);
        }
    }
    n->combined_pattern = cpat;
    n->combined_pattern_len = p - cpat;


    const char *error;
    int erroffset;

    // n->pcre_pattern;
    n->pcre_pattern = pcre_compile(
            n->combined_pattern,              /* the pattern */
            0,                                /* default options */
            &error,               /* for error message */
            &erroffset,           /* for error offset */
            NULL);                /* use default character tables */
    if (n->pcre_pattern == NULL)
    {
        printf("PCRE compilation failed at offset %d: %s\n", erroffset, error);
        return;
    }
}




rnode * rnode_match(rnode * n, char * path, int path_len) {
    if (n->combined_pattern && n->pcre_pattern) {
        info("pcre matching %s on %s\n", n->combined_pattern, path);
        // int ovector_count = (n->edge_len + 1) * 2;
        int ovector_count = 30;
        int ovector[ovector_count];
        int rc;
        rc = pcre_exec(
                n->pcre_pattern,   /* the compiled pattern */
                NULL,              /* no extra data - we didn't study the pattern */
                path,              /* the subject string */
                path_len,          /* the length of the subject */
                0,                 /* start at offset 0 in the subject */
                0,                 /* default options */
                ovector,           /* output vector for substring information */
                ovector_count);      /* number of elements in the output vector */

        info("rc: %d\n", rc );
        if (rc < 0) {
            switch(rc)
            {
                case PCRE_ERROR_NOMATCH: printf("No match\n"); break;
                /*
                Handle other special cases if you like
                */
                default: printf("Matching error %d\n", rc); break;
            }
            // does not match all edges, return NULL;
            return NULL;
        }

        int i;
        for (i = 1; i < rc; i++)
        {
            char *substring_start = path + ovector[2*i];
            int   substring_length = ovector[2*i+1] - ovector[2*i];
            info("%2d: %.*s\n", i, substring_length, substring_start);
            if ( substring_length > 0) {
                int restlen = path_len - ovector[2*i+1]; // fully match to the end
                info("matched item => restlen:%d edges:%d i:%d\n", restlen, n->edge_len, i);
                if (restlen) {
                    return rnode_match( n->edges[i - 1]->child, substring_start + substring_length, restlen);
                }
                return n->edges[i - 1]->child;
            }
        }
        // does not match
        return NULL;
    }

    redge *e = rnode_find_edge_str(n, path, path_len);
    if (e) {
        int len = path_len - e->pattern_len;
        if(len == 0) {
            return e->child;
        } else {
            return rnode_match(e->child, path + e->pattern_len, len);
        }
    }
    return NULL;
}

redge * rnode_find_edge_str(rnode * n, char * str, int str_len) {
    redge *e;
    for ( int i = 0 ; i < n->edge_len ; i++ ) {
        e = n->edges[i];
        char *p = e->pattern;
        while ( *p == *str ) {
            p++;
        }
        if ( p - e->pattern == e->pattern_len ) {
            return e;
        }
    }
    return NULL;
}


rnode * rnode_lookup(rnode * tree, char * path, int path_len) {
    token_array * tokens = split_route_pattern(path, path_len);

    rnode * n = tree;
    redge * e = NULL;
    for ( int i = 0 ; i < tokens->len ; i++ ) {
        e = rnode_find_edge(n, token_array_fetch(tokens, i) );
        if (!e) {
            return NULL;
        }
        n = e->child;
    }
    if (n->endpoint) {
        return n;
    }
    return NULL;
}



rnode * rnode_insert_tokens(rnode * tree, token_array * tokens) {
    rnode * n = tree;
    redge * e = NULL;
    for ( int i = 0 ; i < tokens->len ; i++ ) {
        e = rnode_find_edge(n, token_array_fetch(tokens, i) );
        if (e) {
            n = e->child;
            continue;
        }
        // insert node
        rnode * child = rnode_create(3);
        rnode_add_child(n, strdup(token_array_fetch(tokens,i)) , child);
        n = child;
    }
    n->endpoint++;
    return n;
}

rnode * rnode_insert_route(rnode *tree, char *route)
{
    return rnode_insert_routel(tree, route, strlen(route) );
}

rnode * rnode_insert_routel(rnode *tree, char *route, int route_len)
{
    rnode * n = tree;
    redge * e = NULL;

    char * p = route;

    /* length of common prefix */
    int dl = 0;
    for( int i = 0 ; i < n->edge_len ; i++ ) {
        dl = strndiff(route, n->edges[i]->pattern, n->edges[i]->pattern_len);

        // printf("dl: %d   %s vs %s\n", dl, route, n->edges[i]->pattern );

        // no common, consider insert a new edge
        if ( dl > 0 ) {
            e = n->edges[i];
            break;
        }
    }

    // branch the edge at correct position (avoid broken slugs)
    char *slug_s = strchr(route, '{');
    char *slug_e = strchr(route, '}');
    if ( slug_s && slug_e ) {
        if ( dl > (slug_s - route) && dl < (slug_e - route) ) {
            // break before '{'
            dl = slug_s - route;
        }
    }

    if ( dl == 0 ) {
        // not found, we should just insert a whole new edge
        rnode * child = rnode_create(3);
        rnode_add_child(n, strndup(route, route_len) , child);
        // printf("edge not found, insert one: %s\n", route);

        n = child;
        return n;
    } else if ( dl == e->pattern_len ) {    // fully-equal to the pattern of the edge

        char * subroute = route + dl;
        int    subroute_len = route_len - dl;

        // there are something more we can insert
        if ( subroute_len > 0 ) {
            return rnode_insert_routel(e->child, subroute, subroute_len);
        } else {
            // no more,
            e->child->endpoint++; // make it as an endpoint, TODO: put the route value
            return e->child;
        }

    } else if ( dl < e->pattern_len ) {
        // printf("branch the edge dl: %d\n", dl);


        /* it's partically matched with the pattern,
         * we should split the end point and make a branch here...
         */
        rnode *c2; // child 1, child 2
        redge *e2; // edge 1, edge 2
        char * s2 = route + dl;
        int s2_len = 0;

        redge_branch(e, dl);

        // here is the new edge from.
        c2 = rnode_create(3);
        s2_len = route_len - dl;
        e2 = redge_create(strndup(s2, s2_len), s2_len, c2);
        // printf("edge right: %s\n", e2->pattern);
        rnode_append_edge(e->child, e2);

        // truncate the original edge pattern 
        free(e->pattern);
        e->pattern = strndup(e->pattern, dl);
        e->pattern_len = dl;

        // move n->edges to c1
        c2->endpoint++;
        return c2;
    } else if ( dl > 0 ) {

    } else {
        printf("unexpected condition.");
        return NULL;
    }
    // token_array * t = split_route_pattern(route, strlen(route));
    // return rnode_insert_tokens(tree, t);
    // n->endpoint++;
    return n;
}

bool rnode_has_slug_edges(rnode *n) {
    bool found = FALSE;
    redge *e;
    for ( int i = 0 ; i < n->edge_len ; i++ ) {
        e = n->edges[i];
        e->has_slug = contains_slug(e->pattern);
        if (e->has_slug) 
            found = TRUE;
    }
    return found;
}

void redge_branch(redge *e, int dl) {
    rnode *c1; // child 1, child 2
    redge *e1; // edge 1, edge 2
    char * s1 = e->pattern + dl;
    int s1_len = 0;

    redge **tmp_edges = e->child->edges;
    int   tmp_edge_len = e->child->edge_len;

    // the suffix edge of the leaf
    c1 = rnode_create(3);
    s1_len = e->pattern_len - dl;
    e1 = redge_create(strndup(s1, s1_len), s1_len, c1);
    // printf("edge left: %s\n", e1->pattern);

    // Migrate the child edges to the new edge we just created.
    for ( int i = 0 ; i < tmp_edge_len ; i++ ) {
        rnode_append_edge(c1, tmp_edges[i]);
        e->child->edges[i] = NULL;
    }
    e->child->edge_len = 0;

    rnode_append_edge(e->child, e1);
    c1->endpoint++;
}


redge * redge_create(char * pattern, int pattern_len, rnode * child) {
    redge * edge = (redge*) malloc( sizeof(redge) );
    edge->pattern = pattern;
    edge->pattern_len = pattern_len;
    edge->child = child;
    return edge;
}

void redge_free(redge * e) {
    if (e->pattern) {
        free(e->pattern);
    }
    if ( e->child ) {
        rnode_free(e->child);
    }
}




void rnode_dump(rnode * n, int level) {
    if ( n->edge_len ) {
        if ( n->combined_pattern ) {
            printf(" regexp: %s", n->combined_pattern);
        }
        printf("\n");
        for ( int i = 0 ; i < n->edge_len ; i++ ) {
            redge * e = n->edges[i];
            print_indent(level);
            printf("  |-\"%s\"", e->pattern);

            if (e->has_slug) {
                printf(" slug:");
                printf("%s", compile_slug(e->pattern, e->pattern_len) );
            }

            if ( e->child && e->child->edges ) {
                rnode_dump( e->child, level + 1);
            }
            printf("\n");
        }
    }
}
