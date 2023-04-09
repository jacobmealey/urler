/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <curl/mprintf.h>

#include "version.h"

#ifdef _MSC_VER
#define strncasecmp _strnicmp
#endif

#define OUTPUT_URL      0  /* default */
#define OUTPUT_SCHEME   1
#define OUTPUT_USER     2
#define OUTPUT_PASSWORD 3
#define OUTPUT_OPTIONS  4
#define OUTPUT_HOST     5
#define OUTPUT_PORT     6
#define OUTPUT_PATH     7
#define OUTPUT_QUERY    8
#define OUTPUT_FRAGMENT 9
#define OUTPUT_ZONEID   10
#define NUM_COMPONENTS 11 /* including "url" */

#define PROGNAME        "trurl"

struct var {
  const char *name;
  CURLUPart part;
};

static const struct var variables[] = {
  {"url",      CURLUPART_URL},
  {"scheme",   CURLUPART_SCHEME},
  {"user",     CURLUPART_USER},
  {"password", CURLUPART_PASSWORD},
  {"options",  CURLUPART_OPTIONS},
  {"host",     CURLUPART_HOST},
  {"port",     CURLUPART_PORT},
  {"path",     CURLUPART_PATH},
  {"query",    CURLUPART_QUERY},
  {"fragment", CURLUPART_FRAGMENT},
  {"zoneid",   CURLUPART_ZONEID},
  {NULL, 0}
};

#define ERROR_PREFIX PROGNAME " error: "
#define WARN_PREFIX PROGNAME " note: "

/* error codes */
#define ERROR_FILE    1
#define ERROR_APPEND  2 /* --append mistake */
#define ERROR_ARG     3 /* a command line option misses its argument */
#define ERROR_FLAG    4 /* a command line flag mistake */
#define ERROR_SET     5 /* a --set problem */
#define ERROR_MEM     6 /* out of memory */
#define ERROR_URL     7 /* could not get a URL out of the set components */
#define ERROR_TRIM    8 /* a --trim problem */
#define ERROR_BADURL  9 /* if --verify is set and the URL cannot parse */
#define ERROR_ITER   10 /* unable to find arguments for iterator */

static void warnf(char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fputs(WARN_PREFIX, stderr);
  vfprintf(stderr, fmt, ap);
  fputs("\n", stderr);
  va_end(ap);
}

static void errorf(int exit_code, char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  fputs(ERROR_PREFIX, stderr);
  vfprintf(stderr, fmt, ap);
  fputs("\n" ERROR_PREFIX "Try " PROGNAME " -h for help\n", stderr);
  va_end(ap);
  exit(exit_code);
}


static void help(void)
{
  int i;
  fprintf(stderr, "Usage: " PROGNAME " [options] [URL]\n"
          "  -a, --append [component]=[data] - append data to component\n"
          "  --accept-space               - give in to this URL abuse\n"
          "  -f, --url-file [file/-]      - read URLs from file or stdin\n"
          "  -g, --get [{component}s]     - output component(s)\n"
          "  -h, --help                   - this help\n"
          "  --json                       - output URL as JSON\n"
          "  --redirect [URL]             - redirect to this\n"
          "  -s, --set [component]=[data] - set component content\n"
          "  --trim [component]=[what]    - trim component\n"
          "  --url [URL]                  - URL to work with\n"
          "  -v, --version                - show version\n"
          "  --verify                     - return error on (first) bad URL\n"
          " URL COMPONENTS:\n"
          "  "
    );
  for(i=0; i< NUM_COMPONENTS; i++) {
    fprintf(stderr, "%s%s", i?", ":"", variables[i].name);
  }
  fprintf(stderr, "\n");
  exit(1);
}

static void show_version(void)
{
  curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);
  fprintf(stdout, "%s version %s libcurl/%s\n", PROGNAME, TRURL_VERSION_TXT,
          data->version);
  exit(0);
}

struct option {
  struct curl_slist *url_list;
  struct curl_slist *append_path;
  struct curl_slist *append_query;
  struct curl_slist *set_list;
  struct curl_slist *trim_list;
  const char *redirect;
  const char *format;
  FILE *url;
  bool urlopen;
  bool jsonout;
  struct option *iterate;
  bool verify;
  bool accept_space;
  unsigned char output;

  /* -- stats -- */
  unsigned int urls;
};

#define MAX_QPAIRS 1000
char *qpairs[MAX_QPAIRS];
int nqpairs; /* how many is stored */

static void urladd(struct option *o, const char *url)
{
  struct curl_slist *n;
  n = curl_slist_append(o->url_list, url);
  if(n)
    o->url_list = n;
}


/* read URLs from this file/stdin */
static void urlfile(struct option *o, const char *file)
{
  FILE *f;
  if(o->url)
    errorf(ERROR_FLAG, "only one --url-file is supported");
  if(strcmp("-", file)) {
    f = fopen(file, "rt");
    if(!f)
      errorf(ERROR_FILE, "--url-file %s not found", file);
    o->urlopen = true;
  }
  else
    f = stdin;
  o->url = f;
}

static void pathadd(struct option *o, const char *path)
{
  struct curl_slist *n;
  char *urle = curl_easy_escape(NULL, path, 0);
  if(urle) {
    n = curl_slist_append(o->append_path, urle);
    if(n) {
      o->append_path = n;
    }
    free(urle);
  }
}

static void queryadd(struct option *o, const char *query)
{
  struct curl_slist *n;
  char *p = strchr(query, '=');
  char *urle;
  if(p) {
    /* URL encode the left and the right side of the '=' separately */
    char *f1 = curl_easy_escape(NULL, query, p - query);
    char *f2 = curl_easy_escape(NULL, p + 1, 0);
    urle = curl_maprintf("%s=%s", f1, f2);
    curl_free(f1);
    curl_free(f2);
  }
  else
    urle = curl_easy_escape(NULL, query, 0);
  if(urle) {
    n = curl_slist_append(o->append_query, urle);
    if(n) {
      o->append_query = n;
    }
    free(urle);
  }
}

static void appendadd(struct option *o,
                      const char *arg)
{
  if(!strncasecmp("path=", arg, 5))
    pathadd(o, arg + 5);
  else if(!strncasecmp("query=", arg, 6))
    queryadd(o, arg + 6);
  else
    errorf(ERROR_APPEND, "--append unsupported component: %s", arg);
}

static void setadd(struct option *o,
                   const char *set) /* [component]=[data] */
{
  struct curl_slist *n;
  do {
    n = curl_slist_append(o->set_list, set);
    if(n)
      o->set_list = n;
    o = o->iterate;
  } while(o);
  
}

static void trimadd(struct option *o,
                    const char *trim) /* [component]=[data] */
{
  struct curl_slist *n;
  n = curl_slist_append(o->trim_list, trim);
  if(n)
    o->trim_list = n;
}

static bool checkoptarg(const char *str,
                        const char *given,
                        const char *arg)
{
  if(!strcmp(str, given)) {
    if(!arg)
      errorf(ERROR_ARG, "Missing argument for %s", str);
    return true;
  }
  return false;
}



/* returns address of most recently added option */
struct option *addoptiter(struct option *o){
    while(o->iterate != NULL) {
        o = o->iterate;
    }
    struct option *opt = malloc(sizeof(struct option));
    memcpy(opt, o, sizeof(struct option));
    o->iterate = opt;
    opt->iterate = NULL;
    return opt;
}

struct option *optgetend(struct option *o) {
    struct option *tmp = o;
    while(tmp->iterate != NULL){
        tmp = tmp->iterate;
    }
    return tmp;
}



/* cleans up the options list */
static int optcleanup(struct option *o) {
    if(o->iterate) {
      optcleanup(o->iterate);
    }

    curl_slist_free_all(o->set_list);
    free(o);
    return 0;
}

void print_slist(struct curl_slist *list) {

    if(list == NULL) {
        return ;
    }
    /* loop through to find the last item */
    int i = 0;
    do {
        i++;
        list = list->next;
    } while(list);
}
struct curl_slist *slist_clone(struct curl_slist *list) {
    struct curl_slist *item = NULL;

    if(list == NULL) {
        return item;
    }
    /* loop through to find the last item */
    do {
        item = curl_slist_append(item, list->data);
        if(item == NULL) return item;
        list = list->next;
    } while(list);
    

    return item;
}

/* clones the options list, returns the start of the list */
struct option* cloneopts(struct option *o) {
  struct option *new_opt = malloc(sizeof(struct option));
  struct option *tmp;
  memcpy(new_opt, o, sizeof(struct option));
  new_opt->iterate = NULL;
  int n = 0;
  do{
      tmp = addoptiter(new_opt);
      memcpy(tmp, o, sizeof(struct option));
      tmp->set_list = slist_clone(o->set_list);
      tmp->iterate = NULL;
      o = o->iterate;
      n++;
  } while(o);
  return new_opt;
}

static int iterate(struct option *op, const char *arg) {
    int offset = 0; /* offset from start to the beginning of the arguments */
    int arg_str_len = strlen(arg); /* total length of arguments */
    char buffer[4096];
    memset(buffer, '\0', 4096);
    
    /* check which paramter is being iterated */
    if(!strncmp("hosts=", arg, 5)) {
        strncpy(buffer, "host=", 6);
        offset = 5;
    } else if(!strncmp("ports=", arg, 6)) {
        strncpy(buffer, "port=", 6);
        offset = 5;
    } else if(!strncmp("schemes=", arg, 8)) {
        strncpy(buffer, "scheme=", 8);
        offset = 7;
    }

    if(offset == 0 || offset + 1 >= arg_str_len){
        errorf(ERROR_ITER, "Missing arguments for iterator %s", arg);
    }
    
    //struct option *tmp = cloneopts(op);
    //struct option *tmp = op;

    /* parse individual tokens from arg */
    offset += 1;
    const char *ptr = &arg[offset];
    const char *_arg = ptr;
    struct option *new_opt;
    
    new_opt = op;

    //new_opt = *tmp;
    ptr = &arg[offset];
    _arg = ptr;
    while(*ptr != '\0') {
        bool set = false;
        if(*ptr == ' ' ) {
            strncpy(buffer + offset - 1, _arg, ptr - _arg);
            buffer[offset + ptr - _arg - 1] = '\0';
            _arg = ptr + 1;
            set = true;
        } else if(*(ptr + 1)  == '\0') {
            strncpy(buffer + offset - 1, _arg, ptr - _arg + 1);
            buffer[offset + ptr - _arg] = '\0';
            set = true;
        }
        if(set){
            set = false;
            setadd(new_opt, buffer);
            if(*(ptr + 1) != '\0'){
                new_opt = addoptiter(op);
                new_opt->set_list = slist_clone(op->set_list);
                new_opt->iterate = NULL;
            }
        }
        ptr++;
    }

   return 0;
}

static int getarg(struct option *op,
                  const char *flag,
                  const char *arg,
                  bool *usedarg)
{
  *usedarg = false;

  if(!strcmp("-v", flag) || !strcmp("--version", flag))
    show_version();
  else if(!strcmp("-h", flag) || !strcmp("--help", flag))
    help();
  else if(checkoptarg("--url", flag, arg)) {
    urladd(op, arg);
    *usedarg = 1;
  }
  else if(checkoptarg("-f", flag, arg) ||
          checkoptarg("--url-file", flag, arg)) {
    urlfile(op, arg);
    *usedarg = 1;
  }
  else if(checkoptarg("-a", flag, arg) ||
          checkoptarg("--append", flag, arg)) {
    appendadd(op, arg);
    *usedarg = 1;
  }
  else if(checkoptarg("-s", flag, arg) ||
          checkoptarg("--set", flag, arg)) {
    setadd(op, arg);
    *usedarg = 1;
  }
  else if(checkoptarg("--redirect", flag, arg)) {
    if(op->redirect)
      errorf(ERROR_FLAG, "only one --redirect is supported");
    op->redirect = arg;
    *usedarg = 1;
  }
  else if(checkoptarg("--trim", flag, arg)) {
    trimadd(op, arg);
    *usedarg = 1;
  }
  else if(checkoptarg("-g", flag, arg) ||
          checkoptarg("--get", flag, arg)) {
    if(op->format)
      errorf(ERROR_FLAG, "only one --get is supported");
    op->format = arg;
    *usedarg = 1;
  }
  else if(!strcmp("--json", flag))
    op->jsonout = true;
  else if(checkoptarg("--iterate", flag, arg)) {
      if(op->iterate)
          errorf(ERROR_ITER, "only one --iterate is supported");
      iterate(op, arg);
      *usedarg = 1;
  }
  else if(!strcmp("--verify", flag))
    op->verify = true;
  else if(!strcmp("--accept-space", flag))
    op->accept_space = true;
  else
    return 1;  /* unrecognized option */
  return 0;
}

static void get(struct option *op, CURLU *uh)
{
  FILE *stream = stdout;
  const char *ptr = op->format;
  bool done = false;

  while(ptr && *ptr && !done) {
    if('{' == *ptr) {
      if('{' == ptr[1]) {
        /* an escaped {-letter */
        fputc('{', stream);
        ptr += 2;
      }
      else {
        /* this is meant as a variable to output */
        char *end;
        size_t vlen;
        int i;
        bool urldecode = true;
        end = strchr(ptr, '}');
        ptr++; /* pass the { */
        if(!end) {
          /* syntax error */
          continue;
        }
        /* {path} {:path} */
        if(*ptr == ':') {
          urldecode = false;
          ptr++;
        }
        vlen = end - ptr;
        for(i = 0; variables[i].name; i++) {
          if((strlen(variables[i].name) == vlen) &&
             !strncasecmp(ptr, variables[i].name, vlen)) {
            char *nurl;
            CURLUcode rc;
            rc = curl_url_get(uh, variables[i].part, &nurl,
                              CURLU_DEFAULT_PORT|
                              CURLU_NO_DEFAULT_PORT|
                              (urldecode?CURLU_URLDECODE:0));
            switch(rc) {
            case CURLUE_OK:
              fprintf(stream, "%s", nurl);
              curl_free(nurl);
            case CURLUE_NO_SCHEME:
            case CURLUE_NO_USER:
            case CURLUE_NO_PASSWORD:
            case CURLUE_NO_OPTIONS:
            case CURLUE_NO_HOST:
            case CURLUE_NO_PORT:
            case CURLUE_NO_QUERY:
            case CURLUE_NO_FRAGMENT:
            case CURLUE_NO_ZONEID:
              /* silently ignore */
              break;
            default:
              fprintf(stderr, PROGNAME ": %s (%s)\n", curl_url_strerror(rc),
                      variables[i].name);
              break;
            }
          }
        }
        ptr = end + 1; /* pass the end */
      }
    }
    else if('\\' == *ptr && ptr[1]) {
      switch(ptr[1]) {
      case 'r':
        fputc('\r', stream);
        break;
      case 'n':
        fputc('\n', stream);
        break;
      case 't':
        fputc('\t', stream);
        break;
      default:
        /* unknown, just output this */
        fputc(*ptr, stream);
        fputc(ptr[1], stream);
        break;
      }
      ptr += 2;
    }
    else {
      fputc(*ptr, stream);
      ptr++;
    }
  }
  fputc('\n', stream);
}

static void set(CURLU *uh,
                struct option *o)
{
  struct curl_slist *node;
  bool varset[NUM_COMPONENTS];
  memset(varset, 0, sizeof(varset));
  //printf("node list in set: %p\n", o->set_list);
  for(node =  o->set_list; node; node=node->next) {
    char *set = node->data;
    int i;
    char *ptr = strchr(set, '=');
    if(ptr && (ptr > set)) {
      size_t vlen = ptr-set;
      bool urlencode = true;
      bool found = false;
      if(ptr[-1] == ':') {
        urlencode = false;
        vlen--;
      }
      for(i=0; variables[i].name; i++) {
        if((strlen(variables[i].name) == vlen) &&
           !strncasecmp(set, variables[i].name, vlen)) {
          if(varset[i] && !o)
            errorf(ERROR_SET, "A component can only be set once per URL (%s)",
                   variables[i].name);
          curl_url_set(uh, variables[i].part, ptr[1] ? &ptr[1] : NULL,
                       CURLU_NON_SUPPORT_SCHEME|
                       (urlencode ? CURLU_URLENCODE : 0) );
          found = true;
          varset[i] = true;
          break;
        }
      }
      if(!found)
        errorf(ERROR_SET, "Set unknown component: %s", set);
    }
    else
      errorf(ERROR_SET, "invalid --set syntax: %s", set);
  }
}

static void jsonString(FILE *stream, const char *in, bool lowercase)
{
  const char *i = in;
  const char *in_end = in + strlen(in);

  fputc('\"', stream);
  for(; i < in_end; i++) {
    switch(*i) {
    case '\\':
      fputs("\\\\", stream);
      break;
    case '\"':
      fputs("\\\"", stream);
      break;
    case '\b':
      fputs("\\b", stream);
      break;
    case '\f':
      fputs("\\f", stream);
      break;
    case '\n':
      fputs("\\n", stream);
      break;
    case '\r':
      fputs("\\r", stream);
      break;
    case '\t':
      fputs("\\t", stream);
      break;
    default:
      if (*i < 32) {
        fprintf(stream, "u%04x", *i);
      }
      else {
        char out = *i;
        if(lowercase && (out >= 'A' && out <= 'Z'))
          /* do not use tolower() since that's locale specific */
          out |= ('a' - 'A');
        fputc(out, stream);
      }
      break;
    }
  }
  fputc('\"', stream);
}

static void json(struct option *o, CURLU *uh)
{
  int i;
  (void)o;
  printf("%s  {\n", o->urls?",\n":"");
  for(i = 0; variables[i].name; i++) {
    char *nurl;
    CURLUcode rc = curl_url_get(uh, variables[i].part, &nurl,
                                (i?CURLU_DEFAULT_PORT:0)|
                                CURLU_URLDECODE);
    if(!rc) {
      if(i)
        fputs(",\n", stdout);
      printf("    \"%s\": ", variables[i].name);
      jsonString(stdout, nurl, false);
    }
  }
  fputs("\n  }", stdout);
}

/* --trim query="utm_*" */
static void trim(struct option *o)
{
  struct curl_slist *node;
  for(node = o->trim_list; node; node=node->next) {
    char *instr = node->data;
    if(strncasecmp(instr, "query", 5))
      /* for now we can only trim query components */
      errorf(ERROR_TRIM, "Unsupported trim component: %s", instr);
    char *ptr = strchr(instr, '=');
    if(ptr && (ptr > instr)) {
      /* 'ptr' should be a fixed string or a pattern ending with an
         asterisk */
      size_t inslen;
      bool pattern;
      int i;

      ptr++; /* pass the = */
      inslen = strlen(ptr);
      pattern = ptr[inslen - 1] == '*';
      if(pattern)
        inslen--;

      for(i=0 ; i < nqpairs; i++) {
        char *q = qpairs[i];
        char *sep = strchr(q, '=');
        size_t qlen;
        if(sep)
          qlen = sep - q;
        else
          qlen = strlen(q);

        if((pattern && (inslen <= qlen) &&
            !strncasecmp(q, ptr, inslen)) ||
           (!pattern && (inslen == qlen) &&
            !strncasecmp(q, ptr, inslen))) {
          /* this qpair should be stripped out */
          free(qpairs[i]);
          qpairs[i] = strdup(""); /* marked as deleted */
        }
      }
    }
  }
}

/* memdup the amount and add a trailing zero */
char *memdupzero(char *source, size_t len)
{
  char *p = malloc(len + 1);
  if(p) {
    memcpy(p, source, len);
    p[len] = 0;
    return p;
  }
  return NULL;
}

static void freeqpairs(void)
{
  int i;
  for(i=0; i<nqpairs; i++) {
    free(qpairs[i]);
    qpairs[i] = NULL;
  }
  nqpairs = 0;
}

static char *addqpair(char *pair, size_t len)
{
  char *p = NULL;
  if(nqpairs < MAX_QPAIRS) {
    p = memdupzero(pair, len);
    if(p)
      qpairs[nqpairs++] = p;
  }
  else
    warnf("too many query pairs");
  return p;
}

/* convert the query string into an array of name=data pair */
static void extractqpairs(CURLU *uh)
{
  char *q = NULL;
  memset(qpairs, 0, sizeof(qpairs));
  nqpairs = 0;
  /* extract the query */
  if(!curl_url_get(uh, CURLUPART_QUERY, &q, 0)) {
    char *p = q;
    char *amp;
    while(*p) {
      size_t len;
      amp = strchr(p, '&');
      if(!amp)
        len = strlen(p);
      else
        len = amp - p;
      addqpair(p, len);
      if(amp)
        p = amp + 1;
      else
        break;
    }
  }
  curl_free(q);
}

void qpair2query(CURLU *uh)
{
  int i;
  int rc;
  char *nq=NULL;
  for(i=0; i<nqpairs; i++) {
    nq = curl_maprintf("%s%s%s", nq?nq:"",
                       (nq && *nq && *qpairs[i])? "&": "", qpairs[i]);
  }
  if(nq) {
    rc = curl_url_set(uh, CURLUPART_QUERY, nq, 0);
    if(rc)
      warnf("internal problem");
  }
  curl_free(nq);
}

static void singleurl(struct option *o,
                      const char *url) /* might be NULL */
{
    struct curl_slist *p;
    CURLU *uh = curl_url();
    if(!uh)
      errorf(ERROR_MEM, "out of memory");
    if(url) {
      CURLUcode rc =
        curl_url_set(uh, CURLUPART_URL, url,
                     CURLU_GUESS_SCHEME|CURLU_NON_SUPPORT_SCHEME|
                     (o->accept_space ?
                      (CURLU_ALLOW_SPACE|CURLU_URLENCODE) : 0));
      if(rc) {
        if(o->verify)
          errorf(ERROR_BADURL, "%s [%s]", curl_url_strerror(rc), url);
        warnf("%s [%s]", curl_url_strerror(rc), url);
        return;
      }
      else {
        if(o->redirect)
          curl_url_set(uh, CURLUPART_URL, o->redirect,
                       CURLU_GUESS_SCHEME|CURLU_NON_SUPPORT_SCHEME);
      }
    }
    /* set everything */
    set(uh, o);

    /* append path segments */
    for(p = o->append_path; p; p=p->next) {
      char *apath = p->data;
      char *opath;
      char *npath;
      size_t olen;
      /* extract the current path */
      curl_url_get(uh, CURLUPART_PATH, &opath, 0);

      /* does the existing path end with a slash, then don't
         add one inbetween */
      olen = strlen(opath);

      /* append the new segment */
      npath = curl_maprintf("%s%s%s", opath,
                            opath[olen-1] == '/' ? "" : "/",
                            apath);
      if(npath) {
        /* set the new path */
        curl_url_set(uh, CURLUPART_PATH, npath, 0);
      }
      curl_free(npath);
      curl_free(opath);
    }

    extractqpairs(uh);

    /* append query segments */
    for(p = o->append_query; p; p=p->next) {
      addqpair(p->data, strlen(p->data));
    }

    /* trim parts */
    trim(o);

    /* put the query back */
    qpair2query(uh);

    if(o->jsonout)
      json(o, uh);
    else if(o->format) {
      /* custom output format */
      get(o, uh);
    }
    else {
      /* default output is full URL */
      char *nurl = NULL;
      if(!curl_url_get(uh, CURLUPART_URL, &nurl, CURLU_NO_DEFAULT_PORT)) {
        printf("%s\n", nurl);
        curl_free(nurl);
      }
      else {
        errorf(ERROR_URL, "not enough input for a URL");
      }
    }

    freeqpairs();

    o->urls++;
    curl_url_cleanup(uh);
}

int main(int argc, const char **argv)
{
  int exit_status = 0;
  struct option o;
  struct curl_slist *node;
  memset(&o, 0, sizeof(o));
  curl_global_init(CURL_GLOBAL_ALL);

  for(argc--, argv++; argc > 0; argc--, argv++) {
    bool usedarg = false;
    if((argv[0][0] == '-' && argv[0][1] != '-') ||
       /* single-dash prefix */
       (argv[0][0] == '-' && argv[0][1] == '-')) {
      /* dash-dash prefixed */
      if(getarg(&o, argv[0], argv[1], &usedarg))
        errorf(ERROR_FLAG, "unknown option: %s", argv[0]);
    }
    else {
      /* this is a URL */
      urladd(&o, argv[0]);
    }
    if(usedarg) {
      /* skip the parsed argument */
      argc--;
      argv++;
    }
  }

  if(o.jsonout)
    fputs("[\n", stdout);

  if(o.url) {
    /* this is a file to read URLs from */
    char buffer[4096]; /* arbitrary max */
    while(fgets(buffer, sizeof(buffer), o.url)) {
      char *eol = strchr(buffer, '\n');
      if(eol && (eol > buffer)) {
        if(eol[-1] == '\r')
          /* CRLF detected */
          eol--;
        *eol = 0; /* end of URL */
        
        struct option *opt = &o;
        while(opt->iterate != NULL){
           singleurl(opt, buffer);
           opt= opt->iterate;
        }
      }
      else {
        /* no newline or no content, skip */
      }
    }
    if(o.urlopen)
      fclose(o.url);
  }
  else {
    /* not reading URLs from a file */
    node = o.url_list;
    do {
      struct option *opt = &o;// cloneopts(&o);
      if(node) {
        const char *url = node->data;
        while(opt){
           singleurl(opt, url);
           opt = opt->iterate;
        }
        node = node->next;
      }
      else
          while(opt) {
              singleurl(opt, NULL);
              opt = opt->iterate;
          }

    } while(node);
  }
  if(o.jsonout)
    fputs("\n]\n", stdout);
  /* we're done with libcurl, so clean it up */
  curl_slist_free_all(o.url_list);
  curl_slist_free_all(o.set_list);
  curl_slist_free_all(o.trim_list);
  curl_slist_free_all(o.append_path);
  curl_slist_free_all(o.append_query);
  optcleanup(o.iterate);
  curl_global_cleanup();
  return exit_status;
}
