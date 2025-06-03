#ifndef REQUEST_H
#define REQUEST_H

#define MAX_ROUTE_PARAMS 10
#define MAX_PARAM_LEN    64

typedef struct {
    char name[MAX_PARAM_LEN];
    char value[MAX_PARAM_LEN];
} RequestParam;

typedef struct {
    int            param_count;
    RequestParam   params[MAX_ROUTE_PARAMS];
    char          *body;
} Request;

// Parse the raw body into a Request
Request parse_request(const char *body);
// Free any allocated memory inside Request
void    free_request(Request *req);

#endif // REQUEST_H
