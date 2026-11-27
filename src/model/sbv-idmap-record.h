#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* hostadmin's ID mapping record: a serviceConnectionPoint at
 * CN=hostadmin-idmap,CN=System,<base> whose `keywords` say how the
 * domain's Linux hosts turn users and groups into Unix IDs. Format
 * version 1. Pure parsing, no I/O. */

#define SBV_IDMAP_RECORD_RDN      "hostadmin-idmap"
#define SBV_IDMAP_RECORD_VERSION  1
#define SBV_IDMAP_RECORD_LOW      10000
#define SBV_IDMAP_RECORD_HIGH     999999

typedef enum {
  SBV_IDMAP_BACKEND_RID,   /* ID = low + RID; uidNumber and gidNumber unused */
  SBV_IDMAP_BACKEND_AD,    /* IDs are the RFC 2307 uidNumber and gidNumber */
} SbvIdmapBackend;

typedef struct {
  gint64          version;
  SbvIdmapBackend backend;
  gint64          low;          /* range start, included */
  gint64          high;         /* range end, included */
  char           *source;       /* "rid", "rfc2307" or NULL; informational */
  char           *written_by;   /* or NULL */
  char           *written;      /* YYYY-MM-DD, or NULL */
} SbvIdmapRecord;

/* Parses the `keywords` values of a record. Values without the
 * "hostadmin-idmap-" prefix and names this version doesn't know are
 * skipped. Returns the record, or NULL with `*error` set to a message
 * for the user when the spec calls the record invalid. Caller frees. */
SbvIdmapRecord *sbv_idmap_record_parse      (const char * const *keywords,
                                             char              **error);

/* Copy and free. */
SbvIdmapRecord *sbv_idmap_record_copy       (const SbvIdmapRecord *record);
void            sbv_idmap_record_free       (SbvIdmapRecord *record);

/* Returns the Unix ID a `rid` record gives the account with `rid`, or -1
 * when that falls outside the range, so hosts don't map it. */
gint64          sbv_idmap_record_rid_to_id  (const SbvIdmapRecord *record,
                                             gint64                rid);

/* Returns the RID, the last sub-authority of a binary objectSid, or -1
 * when the SID is too short to hold one. */
gint64          sbv_sid_rid                 (const guchar *sid,
                                             gsize         len);

/* Returns "rid" or "ad". */
const char     *sbv_idmap_backend_name      (SbvIdmapBackend backend);

G_END_DECLS
