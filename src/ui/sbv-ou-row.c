#include "sbv-ou-row.h"

/* Pixels of indent per level below the domain root. */
#define INDENT_PER_LEVEL 18

GtkWidget *
sbv_ou_row_new (SbvOu *ou)
{
  GtkWidget *row = gtk_list_box_row_new ();
  g_object_set_data_full (G_OBJECT (row), "sbv-ou",
                          g_object_ref (ou), g_object_unref);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_top    (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start  (box, 12 + INDENT_PER_LEVEL * (int) sbv_ou_get_depth (ou));
  gtk_widget_set_margin_end    (box, 12);

  SbvOuKind kind = sbv_ou_get_kind (ou);
  GtkWidget *icon = gtk_image_new_from_icon_name (
    kind == SBV_OU_KIND_DOMAIN ? "network-server-symbolic" : "folder-symbolic");
  if (kind == SBV_OU_KIND_CONTAINER)
    gtk_widget_add_css_class (icon, "dim-label");
  gtk_box_append (GTK_BOX (box), icon);

  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_set_hexpand (text, TRUE);

  GtkWidget *name = gtk_label_new (sbv_ou_get_name (ou) ?: "");
  gtk_label_set_xalign (GTK_LABEL (name), 0);
  gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
  if (kind != SBV_OU_KIND_CONTAINER)
    gtk_widget_add_css_class (name, "heading");
  gtk_box_append (GTK_BOX (text), name);

  const char *desc = sbv_ou_get_description (ou);
  const char *sub  = (desc && *desc) ? desc
                   : kind == SBV_OU_KIND_CONTAINER ? "Container"
                   : kind == SBV_OU_KIND_DOMAIN    ? "Domain root"
                   : NULL;
  if (sub) {
    GtkWidget *sub_lbl = gtk_label_new (sub);
    gtk_label_set_xalign (GTK_LABEL (sub_lbl), 0);
    gtk_label_set_ellipsize (GTK_LABEL (sub_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (sub_lbl, "dim-label");
    gtk_widget_add_css_class (sub_lbl, "caption");
    gtk_box_append (GTK_BOX (text), sub_lbl);
  }

  gtk_box_append (GTK_BOX (box), text);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  return row;
}

SbvOu *
sbv_ou_row_get_ou (GtkListBoxRow *row)
{
  return row ? g_object_get_data (G_OBJECT (row), "sbv-ou") : NULL;
}
