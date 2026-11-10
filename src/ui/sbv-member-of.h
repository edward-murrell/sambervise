#pragma once

#include <gtk/gtk.h>
#include "../backend/sbv-connection.h"

G_BEGIN_DECLS

#define SBV_TYPE_MEMBER_OF (sbv_member_of_get_type ())
G_DECLARE_FINAL_TYPE (SbvMemberOf, sbv_member_of, SBV, MEMBER_OF, GtkBox)

/* A "Member Of" section for a user or computer pane. It lists the groups
 * the object belongs to, with a Remove button on each, and an "Add to
 * Group…" button that opens a group picker. Membership changes modify
 * each group's `member` attribute. The "changed" signal fires after a
 * successful add or remove, so the owning panel can reload. */
GtkWidget *sbv_member_of_new (void);

/* Shows the memberships of the object at `dn`. `member_of` holds its
 * memberOf DNs and may be NULL. `primary_group_id` is the primary
 * group's RID, or -1. All arguments are copied. */
void       sbv_member_of_set (SbvMemberOf        *self,
                              SbvConnection      *conn,
                              const char         *dn,
                              const char * const *member_of,
                              gint                primary_group_id);

G_END_DECLS
