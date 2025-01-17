/* GTK - The GIMP Toolkit
 * Copyright (C) 2010 Carlos Garnacho <carlosg@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gtk/css/gtkcss.h>
#include "gtk/css/gtkcsstokenizerprivate.h"
#include "gtk/css/gtkcssparserprivate.h"
#include "gtkcssvalueprivate.h"
#include "gtkcsscolorprivate.h"

G_BEGIN_DECLS

GtkCssValue *   gtk_css_color_value_new_transparent     (void) G_GNUC_PURE;
GtkCssValue *   gtk_css_color_value_new_white           (void) G_GNUC_PURE;
GtkCssValue *   gtk_css_color_value_new_current_color   (void) G_GNUC_PURE;
GtkCssValue *   gtk_css_color_value_new_name            (const char     *name) G_GNUC_PURE;

gboolean        gtk_css_color_value_can_parse           (GtkCssParser   *parser);
GtkCssValue *   gtk_css_color_value_parse               (GtkCssParser   *parser);

GtkCssValue *   gtk_css_color_value_resolve             (GtkCssValue      *color,
                                                         GtkStyleProvider *provider,
                                                         GtkCssValue      *current);
const GdkRGBA * gtk_css_color_value_get_rgba            (const GtkCssValue *color) G_GNUC_CONST;

GtkCssValue *   gtk_css_color_value_new_color           (GtkCssColorSpace color_space,
                                                         gboolean         serialize_as_rgb,
                                                         const float      values[4],
                                                         gboolean         missing[4]) G_GNUC_PURE;

const GtkCssColor *
                gtk_css_color_value_get_color           (const GtkCssValue *color) G_GNUC_CONST;


G_END_DECLS

