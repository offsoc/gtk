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

#include "config.h"

#include "gtkcsscolorvalueprivate.h"

#include "gtkcssstylepropertyprivate.h"
#include "gtkprivate.h"
#include "gtkstylepropertyprivate.h"
#include "gtkcssnumbervalueprivate.h"
#include "gtkcssstyleprivate.h"
#include "gtkstyleproviderprivate.h"

#include "gdk/gdkhslaprivate.h"
#include "gdk/gdkrgbaprivate.h"
#include "gtkcsscolorprivate.h"
#include "gtkcolorutilsprivate.h"


static GtkCssValue * gtk_css_color_value_new_mix (GtkCssValue *color1,
                                                  GtkCssValue *color2,
                                                  double       factor);
static GtkCssValue * gtk_css_color_value_new_literal (const GdkRGBA *color);

typedef enum {
  COLOR_TYPE_LITERAL,
  COLOR_TYPE_COLOR,
  COLOR_TYPE_NAME,
  COLOR_TYPE_SHADE,
  COLOR_TYPE_ALPHA,
  COLOR_TYPE_MIX,
  COLOR_TYPE_CURRENT_COLOR,
} ColorType;

struct _GtkCssValue
{
  GTK_CSS_VALUE_BASE
  guint serialize_as_rgb : 1;
  guint type : 16;
  GdkRGBA rgba;

  union
  {
    char *name;
    GtkCssColor color;

    struct
    {
      GtkCssValue *color;
      double factor;
    } shade, alpha;

    struct
    {
      GtkCssValue *color1;
      GtkCssValue *color2;
      double factor;
    } mix;
  };
};

static void
gtk_css_value_color_free (GtkCssValue *color)
{
  switch (color->type)
    {
    case COLOR_TYPE_NAME:
      g_free (color->name);
      break;

    case COLOR_TYPE_SHADE:
      gtk_css_value_unref (color->shade.color);
      break;

    case COLOR_TYPE_ALPHA:
      gtk_css_value_unref (color->alpha.color);
      break;

    case COLOR_TYPE_MIX:
      gtk_css_value_unref (color->mix.color1);
      gtk_css_value_unref (color->mix.color2);
      break;

    case COLOR_TYPE_LITERAL:
    case COLOR_TYPE_COLOR:
    case COLOR_TYPE_CURRENT_COLOR:
    default:
      break;
    }

  g_free (color);
}

static GtkCssValue *
gtk_css_value_color_get_fallback (guint                 property_id,
                                  GtkCssComputeContext *context)
{
  switch (property_id)
    {
      case GTK_CSS_PROPERTY_BACKGROUND_IMAGE:
      case GTK_CSS_PROPERTY_BORDER_IMAGE_SOURCE:
      case GTK_CSS_PROPERTY_TEXT_SHADOW:
      case GTK_CSS_PROPERTY_ICON_SHADOW:
      case GTK_CSS_PROPERTY_BOX_SHADOW:
        return gtk_css_color_value_new_transparent ();

      case GTK_CSS_PROPERTY_COLOR:
      case GTK_CSS_PROPERTY_BACKGROUND_COLOR:
      case GTK_CSS_PROPERTY_BORDER_TOP_COLOR:
      case GTK_CSS_PROPERTY_BORDER_RIGHT_COLOR:
      case GTK_CSS_PROPERTY_BORDER_BOTTOM_COLOR:
      case GTK_CSS_PROPERTY_BORDER_LEFT_COLOR:
      case GTK_CSS_PROPERTY_OUTLINE_COLOR:
      case GTK_CSS_PROPERTY_CARET_COLOR:
      case GTK_CSS_PROPERTY_SECONDARY_CARET_COLOR:
        return gtk_css_value_compute (_gtk_css_style_property_get_initial_value (_gtk_css_style_property_lookup_by_id (property_id)),
                                       property_id,
                                       context);

      case GTK_CSS_PROPERTY_ICON_PALETTE:
        return gtk_css_value_ref (context->style->core->color);

      default:
        if (property_id < GTK_CSS_PROPERTY_N_PROPERTIES)
          g_warning ("No fallback color defined for property '%s'",
                     _gtk_style_property_get_name (GTK_STYLE_PROPERTY (_gtk_css_style_property_lookup_by_id (property_id))));
        return gtk_css_color_value_new_transparent ();
    }
}

static GtkCssValue *
gtk_css_value_color_compute (GtkCssValue          *value,
                             guint                 property_id,
                             GtkCssComputeContext *context)
{
  GtkCssValue *resolved;

  /* The computed value of the ‘currentColor’ keyword is the computed
   * value of the ‘color’ property. If the ‘currentColor’ keyword is
   * set on the ‘color’ property itself, it is treated as ‘color: inherit’.
   */
  if (property_id == GTK_CSS_PROPERTY_COLOR)
    {
      GtkCssValue *current;

      if (context->parent_style)
        current = context->parent_style->core->color;
      else
        current = NULL;

      resolved = gtk_css_color_value_resolve (value,
                                              context->provider,
                                              current);
    }
  else if (value->type == COLOR_TYPE_LITERAL)
    {
      resolved = gtk_css_value_ref (value);
    }
  else
    {
      GtkCssValue *current = context->style->core->color;

      resolved = gtk_css_color_value_resolve (value,
                                              context->provider,
                                              current);
    }

  if (resolved == NULL)
    return gtk_css_value_color_get_fallback (property_id, context);

  return resolved;
}

static gboolean
gtk_css_value_color_equal (const GtkCssValue *value1,
                           const GtkCssValue *value2)
{
  if (value1->type == COLOR_TYPE_COLOR && value1->color.missing == 0 &&
      value2->type == COLOR_TYPE_LITERAL)
    return gdk_rgba_equal (&value1->rgba, &value2->rgba);

  if (value2->type == COLOR_TYPE_COLOR && value2->color.missing == 0 &&
      value1->type == COLOR_TYPE_LITERAL)
    return gdk_rgba_equal (&value1->rgba, &value2->rgba);

  if (value1->type != value2->type)
    return FALSE;

  switch (value1->type)
    {
    case COLOR_TYPE_LITERAL:
      return gdk_rgba_equal (&value1->rgba, &value2->rgba);

    case COLOR_TYPE_COLOR:
      return gtk_css_color_equal (&value1->color, &value2->color);

    case COLOR_TYPE_NAME:
      return g_str_equal (value1->name, value2->name);

    case COLOR_TYPE_SHADE:
      return value1->shade.factor == value2->shade.factor &&
             gtk_css_value_equal (value1->shade.color,
                                  value2->shade.color);

    case COLOR_TYPE_ALPHA:
      return value1->alpha.factor == value2->alpha.factor &&
             gtk_css_value_equal (value1->alpha.color,
                                  value2->alpha.color);

    case COLOR_TYPE_MIX:
      return value1->mix.factor == value2->mix.factor &&
             gtk_css_value_equal (value1->mix.color1,
                                  value2->mix.color1) &&
             gtk_css_value_equal (value1->mix.color2,
                                  value2->mix.color2);

    case COLOR_TYPE_CURRENT_COLOR:
      return TRUE;

    default:
      g_assert_not_reached ();
      return FALSE;
    }
}

static GtkCssValue *
gtk_css_value_color_transition (GtkCssValue *start,
                                GtkCssValue *end,
                                guint        property_id,
                                double       progress)
{
  return gtk_css_color_value_new_mix (start, end, progress);
}

static inline void
append_color_component (GString           *string,
                        const GtkCssColor *color,
                        guint              idx)
{
  if (gtk_css_color_component_missing (color, idx))
    g_string_append (string, "none");
  else
    g_string_append_printf (string, "%g", gtk_css_color_get_component (color, idx));
}

static void
gtk_css_value_color_print (const GtkCssValue *value,
                           GString           *string)
{
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];

  switch (value->type)
    {
    case COLOR_TYPE_LITERAL:
      gdk_rgba_print (&value->rgba, string);
      break;

    case COLOR_TYPE_COLOR:
      gtk_css_color_print (&value->color, value->serialize_as_rgb, string);
      break;

    case COLOR_TYPE_NAME:
      g_string_append (string, "@");
      g_string_append (string, value->name);
      break;

    case COLOR_TYPE_SHADE:
      g_string_append (string, "shade(");
      gtk_css_value_print (value->shade.color, string);
      g_string_append (string, ", ");
      g_ascii_dtostr (buffer, sizeof (buffer), value->shade.factor);
      g_string_append (string, buffer);
      g_string_append_c (string, ')');
      break;

    case COLOR_TYPE_ALPHA:
      g_string_append (string, "alpha(");
      gtk_css_value_print (value->alpha.color, string);
      g_string_append (string, ", ");
      g_ascii_dtostr (buffer, sizeof (buffer), value->alpha.factor);
      g_string_append (string, buffer);
      g_string_append_c (string, ')');
      break;

    case COLOR_TYPE_MIX:
      g_string_append (string, "mix(");
      gtk_css_value_print (value->mix.color1, string);
      g_string_append (string, ", ");
      gtk_css_value_print (value->mix.color2, string);
      g_string_append (string, ", ");
      g_ascii_dtostr (buffer, sizeof (buffer), value->mix.factor);
      g_string_append (string, buffer);
      g_string_append_c (string, ')');
      break;

    case COLOR_TYPE_CURRENT_COLOR:
      g_string_append (string, "currentcolor");
      break;

    default:
      g_assert_not_reached ();
    }
}

static const GtkCssValueClass GTK_CSS_VALUE_COLOR = {
  "GtkCssColorValue",
  gtk_css_value_color_free,
  gtk_css_value_color_compute,
  gtk_css_value_color_equal,
  gtk_css_value_color_transition,
  NULL,
  NULL,
  gtk_css_value_color_print
};

static void
apply_alpha (const GdkRGBA *in,
             GdkRGBA       *out,
             double         factor)
{
  *out = *in;

  out->alpha = CLAMP (in->alpha * factor, 0, 1);
}

static void
apply_shade (const GdkRGBA *in,
             GdkRGBA       *out,
             double         factor)
{
  GdkHSLA hsla;

  _gdk_hsla_init_from_rgba (&hsla, in);
  _gdk_hsla_shade (&hsla, &hsla, factor);

  _gdk_rgba_init_from_hsla (out, &hsla);
}

static inline double
transition (double start,
            double end,
             double progress)
{
  return start + (end - start) * progress;
}

static void
apply_mix (const GdkRGBA *in1,
           const GdkRGBA *in2,
           GdkRGBA       *out,
           double         factor)
{
  out->alpha = CLAMP (transition (in1->alpha, in2->alpha, factor), 0, 1);

  if (out->alpha <= 0.0)
    {
      out->red = out->green = out->blue = 0.0;
    }
  else
    {
      out->red   = CLAMP (transition (in1->red * in1->alpha, in2->red * in2->alpha, factor), 0,  1) / out->alpha;
      out->green = CLAMP (transition (in1->green * in1->alpha, in2->green * in2->alpha, factor), 0,  1) / out->alpha;
      out->blue  = CLAMP (transition (in1->blue * in1->alpha, in2->blue * in2->alpha, factor), 0,  1) / out->alpha;
    }
}

static GtkCssValue *
gtk_css_color_value_do_resolve (GtkCssValue      *color,
                                GtkStyleProvider *provider,
                                GtkCssValue      *current,
                                GSList           *cycle_list)
{
  GtkCssValue *value;

  gtk_internal_return_val_if_fail (color != NULL, NULL);

  switch (color->type)
    {
    case COLOR_TYPE_LITERAL:
    case COLOR_TYPE_COLOR:
      value = gtk_css_value_ref (color);
      break;

    case COLOR_TYPE_NAME:
      {
        GtkCssValue *named;
        GSList cycle = { color, cycle_list };

        /* If color exists in cycle_list, we're currently resolving it.
         * So we've detected a cycle. */
        if (g_slist_find (cycle_list, color))
          return NULL;

        named = gtk_style_provider_get_color (provider, color->name);
        if (named == NULL)
          return NULL;

        value = gtk_css_color_value_do_resolve (named, provider, current, &cycle);
      }
      break;

    case COLOR_TYPE_SHADE:
      {
        const GdkRGBA *c;
        GtkCssValue *val;
        GdkRGBA shade;

        val = gtk_css_color_value_do_resolve (color->shade.color, provider, current, cycle_list);
        if (val == NULL)
          return NULL;
        c = gtk_css_color_value_get_rgba (val);

        apply_shade (c, &shade, color->shade.factor);

        value = gtk_css_color_value_new_literal (&shade);
        gtk_css_value_unref (val);
      }
      break;

    case COLOR_TYPE_ALPHA:
      {
        const GdkRGBA *c;
        GtkCssValue *val;
        GdkRGBA alpha;

        val = gtk_css_color_value_do_resolve (color->alpha.color, provider, current, cycle_list);
        if (val == NULL)
          return NULL;
        c = gtk_css_color_value_get_rgba (val);

        apply_alpha (c, &alpha, color->alpha.factor);

        value = gtk_css_color_value_new_literal (&alpha);
        gtk_css_value_unref (val);
      }
      break;

    case COLOR_TYPE_MIX:
      {
        const GdkRGBA *color1, *color2;
        GtkCssValue *val1, *val2;
        GdkRGBA res;

        val1 = gtk_css_color_value_do_resolve (color->mix.color1, provider, current, cycle_list);
        if (val1 == NULL)
          return NULL;
        color1 = gtk_css_color_value_get_rgba (val1);

        val2 = gtk_css_color_value_do_resolve (color->mix.color2, provider, current, cycle_list);
        if (val2 == NULL)
          return NULL;
        color2 = gtk_css_color_value_get_rgba (val2);

        apply_mix (color1, color2, &res, color->mix.factor);

        value = gtk_css_color_value_new_literal (&res);
        gtk_css_value_unref (val1);
        gtk_css_value_unref (val2);
      }
      break;

    case COLOR_TYPE_CURRENT_COLOR:
      if (current == NULL)
        current = _gtk_css_style_property_get_initial_value (_gtk_css_style_property_lookup_by_id (GTK_CSS_PROPERTY_COLOR));

      value = gtk_css_value_ref (current);
      break;

    default:
      value = NULL;
      g_assert_not_reached ();
    }

  return value;
}

GtkCssValue *
gtk_css_color_value_resolve (GtkCssValue      *color,
                             GtkStyleProvider *provider,
                             GtkCssValue      *current)
{
  return gtk_css_color_value_do_resolve (color, provider, current, NULL);
}

static GtkCssValue transparent_black_singleton = { &GTK_CSS_VALUE_COLOR, 1, TRUE, FALSE, FALSE, COLOR_TYPE_LITERAL,
                                                   .rgba = {0, 0, 0, 0} };
static GtkCssValue white_singleton             = { &GTK_CSS_VALUE_COLOR, 1, TRUE, FALSE, FALSE, COLOR_TYPE_LITERAL,
                                                   .rgba = {1, 1, 1, 1} };


GtkCssValue *
gtk_css_color_value_new_transparent (void)
{
  return gtk_css_value_ref (&transparent_black_singleton);
}

GtkCssValue *
gtk_css_color_value_new_white (void)
{
  return gtk_css_value_ref (&white_singleton);
}

static GtkCssValue *
gtk_css_color_value_new_literal (const GdkRGBA *color)
{
  GtkCssValue *value;

  g_return_val_if_fail (color != NULL, NULL);

  if (gdk_rgba_equal (color, &white_singleton.rgba))
    return gtk_css_value_ref (&white_singleton);

  if (gdk_rgba_equal (color, &transparent_black_singleton.rgba))
    return gtk_css_value_ref (&transparent_black_singleton);

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->type = COLOR_TYPE_LITERAL;
  value->is_computed = TRUE;
  value->rgba = *color;

  return value;
}

GtkCssValue *
gtk_css_color_value_new_color (GtkCssColorSpace color_space,
                               gboolean         serialize_as_rgb,
                               const float      values[4],
                               gboolean         missing[4])
{
  GtkCssValue *value;
  GtkCssColor tmp;

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->color.color_space = color_space;
  value->is_computed = TRUE;
  value->serialize_as_rgb = serialize_as_rgb;
  value->type = COLOR_TYPE_COLOR;
  gtk_css_color_init_with_missing (&value->color, color_space, values, missing);

  gtk_css_color_convert (&value->color, GTK_CSS_COLOR_SPACE_SRGB, &tmp);

  value->rgba.red = tmp.values[0];
  value->rgba.green = tmp.values[1];
  value->rgba.blue = tmp.values[2];
  value->rgba.alpha = tmp.values[3];

  return value;
}

GtkCssValue *
gtk_css_color_value_new_name (const char *name)
{
  GtkCssValue *value;

  gtk_internal_return_val_if_fail (name != NULL, NULL);

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->type = COLOR_TYPE_NAME;
  value->name = g_strdup (name);

  return value;
}

static GtkCssValue *
gtk_css_color_value_new_shade (GtkCssValue *color,
                               double       factor)
{
  GtkCssValue *value;

  gtk_internal_return_val_if_fail (color->class == &GTK_CSS_VALUE_COLOR, NULL);

  if (color->type == COLOR_TYPE_LITERAL || color->type == COLOR_TYPE_COLOR)
    {
      GdkRGBA c;

      apply_shade (&color->rgba, &c, factor);

      return gtk_css_color_value_new_literal (&c);
    }

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->type = COLOR_TYPE_SHADE;
  value->shade.color = gtk_css_value_ref (color);
  value->shade.factor = factor;

  return value;
}

static GtkCssValue *
gtk_css_color_value_new_alpha (GtkCssValue *color,
                               double       factor)
{
  GtkCssValue *value;

  gtk_internal_return_val_if_fail (color->class == &GTK_CSS_VALUE_COLOR, NULL);

  if (color->type == COLOR_TYPE_LITERAL || color->type == COLOR_TYPE_COLOR)
    {
      GdkRGBA c;

      apply_alpha (&color->rgba, &c, factor);

      return gtk_css_color_value_new_literal (&c);
    }

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->type = COLOR_TYPE_ALPHA;
  value->alpha.color = gtk_css_value_ref (color);
  value->alpha.factor = factor;

  return value;
}

static GtkCssValue *
gtk_css_color_value_new_mix (GtkCssValue *color1,
                             GtkCssValue *color2,
                             double       factor)
{
  GtkCssValue *value;

  gtk_internal_return_val_if_fail (color1->class == &GTK_CSS_VALUE_COLOR, NULL);
  gtk_internal_return_val_if_fail (color2->class == &GTK_CSS_VALUE_COLOR, NULL);

  if ((color1->type == COLOR_TYPE_LITERAL || color1->type == COLOR_TYPE_COLOR) &&
      (color2->type == COLOR_TYPE_LITERAL || color2->type == COLOR_TYPE_COLOR))
    {
      GdkRGBA result;

      apply_mix (&color1->rgba, &color2->rgba, &result, factor);

      return gtk_css_color_value_new_literal (&result);

    }

  value = gtk_css_value_new (GtkCssValue, &GTK_CSS_VALUE_COLOR);
  value->type = COLOR_TYPE_MIX;
  value->mix.color1 = gtk_css_value_ref (color1);
  value->mix.color2 = gtk_css_value_ref (color2);
  value->mix.factor = factor;

  return value;
}

GtkCssValue *
gtk_css_color_value_new_current_color (void)
{
  static GtkCssValue current_color = { &GTK_CSS_VALUE_COLOR, 1, FALSE, FALSE, FALSE, COLOR_TYPE_CURRENT_COLOR, };

  return gtk_css_value_ref (&current_color);
}

typedef struct
{
  GtkCssValue *color;
  GtkCssValue *color2;
  double       value;
} ColorFunctionData;

static guint
parse_color_mix (GtkCssParser *parser,
                 guint         arg,
                 gpointer      data_)
{
  ColorFunctionData *data = data_;

  switch (arg)
  {
    case 0:
      data->color = gtk_css_color_value_parse (parser);
      if (data->color == NULL)
        return 0;
      return 1;

    case 1:
      data->color2 = gtk_css_color_value_parse (parser);
      if (data->color2 == NULL)
        return 0;
      return 1;

    case 2:
      if (!gtk_css_parser_consume_number (parser, &data->value))
        return 0;
      return 1;

    default:
      g_return_val_if_reached (0);
  }
}

static guint
parse_color_number (GtkCssParser *parser,
                    guint         arg,
                    gpointer      data_)
{
  ColorFunctionData *data = data_;

  switch (arg)
  {
    case 0:
      data->color = gtk_css_color_value_parse (parser);
      if (data->color == NULL)
        return 0;
      return 1;

    case 1:
      if (!gtk_css_parser_consume_number (parser, &data->value))
        return 0;
      return 1;

    default:
      g_return_val_if_reached (0);
  }
}

gboolean
gtk_css_color_value_can_parse (GtkCssParser *parser)
{
  /* This is way too generous, but meh... */
  return gtk_css_parser_has_token (parser, GTK_CSS_TOKEN_IDENT)
      || gtk_css_parser_has_token (parser, GTK_CSS_TOKEN_AT_KEYWORD)
      || gtk_css_parser_has_token (parser, GTK_CSS_TOKEN_HASH_ID)
      || gtk_css_parser_has_token (parser, GTK_CSS_TOKEN_HASH_UNRESTRICTED)
      || gtk_css_parser_has_function (parser, "lighter")
      || gtk_css_parser_has_function (parser, "darker")
      || gtk_css_parser_has_function (parser, "shade")
      || gtk_css_parser_has_function (parser, "alpha")
      || gtk_css_parser_has_function (parser, "mix")
      || gtk_css_parser_has_function (parser, "hsl")
      || gtk_css_parser_has_function (parser, "hsla")
      || gtk_css_parser_has_function (parser, "rgb")
      || gtk_css_parser_has_function (parser, "rgba")
      || gtk_css_parser_has_function (parser, "hwb")
      || gtk_css_parser_has_function (parser, "oklab")
      || gtk_css_parser_has_function (parser, "oklch")
      || gtk_css_parser_has_function (parser, "color");
}

typedef struct
{
  GdkRGBA *rgba;
  gboolean use_percentages;
  gboolean missing[4];
} ParseRGBAData;

typedef enum
{
  COLOR_SYNTAX_DETECTING,
  COLOR_SYNTAX_MODERN,
  COLOR_SYNTAX_LEGACY,
} ColorSyntax;

static gboolean
parse_rgb_channel_value (GtkCssParser  *parser,
                         float         *value,
                         gboolean      *missing,
                         ColorSyntax   *syntax,
                         ParseRGBAData *data)
{
  gboolean has_percentage =
    gtk_css_token_is (gtk_css_parser_get_token (parser), GTK_CSS_TOKEN_PERCENTAGE);

  switch (*syntax)
  {
    case COLOR_SYNTAX_DETECTING:
      data->use_percentages = has_percentage;
      break;

    case COLOR_SYNTAX_LEGACY:
      if (data->use_percentages != has_percentage)
        {
          gtk_css_parser_error_syntax (parser, "Legacy color syntax doesn't allow mixing numbers and percentages");
          return FALSE;
        }

      break;

    case COLOR_SYNTAX_MODERN:
      break;

    default:
      g_assert_not_reached ();
  }

  if (*syntax != COLOR_SYNTAX_LEGACY &&
      gtk_css_parser_try_ident (parser, "none"))
    {
      *syntax = COLOR_SYNTAX_MODERN;
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, GTK_CSS_PARSE_NUMBER | GTK_CSS_PARSE_PERCENT);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 255);
      *value = CLAMP (*value, 0.0, 255.0) / 255.0;

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_alpha_value (GtkCssParser *parser,
                   float        *value,
                   gboolean     *missing,
                   ColorSyntax  *syntax)
{
  GtkCssNumberParseFlags flags = GTK_CSS_PARSE_NUMBER;

  if (*syntax == COLOR_SYNTAX_MODERN)
    flags |= GTK_CSS_PARSE_PERCENT;

  if (*syntax != COLOR_SYNTAX_LEGACY &&
      gtk_css_parser_try_ident (parser, "none"))
    {
      *syntax = COLOR_SYNTAX_MODERN;
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 1);
      *value = CLAMP (*value, 0.0, 1.0);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_hsl_channel_value (GtkCssParser  *parser,
                         float         *value,
                         gboolean      *missing,
                         ColorSyntax   *syntax)
{
  if (*syntax != COLOR_SYNTAX_LEGACY &&
      gtk_css_parser_try_ident (parser, "none"))
    {
      *syntax = COLOR_SYNTAX_MODERN;
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_PERCENT;
      GtkCssValue *val;

      *missing = FALSE;

      if (*syntax == COLOR_SYNTAX_MODERN)
        flags |= GTK_CSS_PARSE_NUMBER;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 100);
      *value = CLAMP (*value, 0.0, 100.0);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_hwb_channel_value (GtkCssParser  *parser,
                         float         *value,
                         gboolean      *missing,
                         ColorSyntax   *syntax)
{
  if (*syntax != COLOR_SYNTAX_LEGACY &&
      gtk_css_parser_try_ident (parser, "none"))
    {
      *syntax = COLOR_SYNTAX_MODERN;
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_PERCENT | GTK_CSS_PARSE_NUMBER;
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 100);
      *value = CLAMP (*value, 0.0, 100.0);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_hue_value (GtkCssParser *parser,
                 float        *value,
                 gboolean     *missing,
                 ColorSyntax  *syntax)
{
  if (*syntax != COLOR_SYNTAX_LEGACY &&
      gtk_css_parser_try_ident (parser, "none"))
    {
      *syntax = COLOR_SYNTAX_MODERN;
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssValue *hue;

      *missing = FALSE;

      hue = gtk_css_number_value_parse (parser, GTK_CSS_PARSE_NUMBER | GTK_CSS_PARSE_ANGLE);
      if (hue == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (hue, 360);
      *value = fmod (*value, 360);
      if (*value < 0)
        *value += 360;

      gtk_css_value_unref (hue);
    }

  return TRUE;
}

static gboolean
parse_ok_L_value (GtkCssParser  *parser,
                  float         *value,
                  gboolean      *missing)
{
  if (gtk_css_parser_try_ident (parser, "none"))
    {
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_PERCENT | GTK_CSS_PARSE_NUMBER;
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 1);
      *value = CLAMP (*value, 0.0, 1.0);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_ok_C_value (GtkCssParser  *parser,
                  float         *value,
                  gboolean      *missing)
{
  if (gtk_css_parser_try_ident (parser, "none"))
    {
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_PERCENT | GTK_CSS_PARSE_NUMBER;
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 0.4);
      *value = MAX (*value, 0.0);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static gboolean
parse_ok_ab_value (GtkCssParser  *parser,
                   float         *value,
                   gboolean      *missing)
{
  if (gtk_css_parser_try_ident (parser, "none"))
    {
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_PERCENT | GTK_CSS_PARSE_NUMBER;
      GtkCssValue *val;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 0.4);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static guint
parse_rgba_color_channel (GtkCssParser *parser,
                          guint         arg,
                          ColorSyntax  *syntax,
                          gpointer      data)
{
  ParseRGBAData *rgba_data = data;
  GdkRGBA *rgba = rgba_data->rgba;
  gboolean *missing = rgba_data->missing;

  switch (arg)
    {
    case 0:
      return parse_rgb_channel_value (parser, &rgba->red, &missing[0], syntax, rgba_data);

    case 1:
      return parse_rgb_channel_value (parser, &rgba->green, &missing[1], syntax, rgba_data);

    case 2:
      return parse_rgb_channel_value (parser, &rgba->blue, &missing[2], syntax, rgba_data);

    case 3:
      return parse_alpha_value (parser, &rgba->alpha, &missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

typedef struct
{
  GdkHSLA *hsla;
  gboolean missing[4];
} ParseHSLAData;

static guint
parse_hsla_color_channel (GtkCssParser *parser,
                          guint         arg,
                          ColorSyntax  *syntax,
                          gpointer      data)
{
  ParseHSLAData *hsla_data = data;
  GdkHSLA *hsla = hsla_data->hsla;
  gboolean *missing = hsla_data->missing;

  switch (arg)
    {
    case 0:
      return parse_hue_value (parser, &hsla->hue, &missing[0], syntax);

    case 1:
      return parse_hsl_channel_value (parser, &hsla->saturation, &missing[1], syntax);

    case 2:
      return parse_hsl_channel_value (parser, &hsla->lightness, &missing[2], syntax);

    case 3:
      return parse_alpha_value (parser, &hsla->alpha, &missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

typedef struct {
  float hue, white, black, alpha;
  gboolean missing[4];
} HwbData;

static guint
parse_hwb_color_channel (GtkCssParser *parser,
                         guint         arg,
                         ColorSyntax  *syntax,
                         gpointer      data)
{
  HwbData *hwb = data;

  switch (arg)
    {
    case 0:
      return parse_hue_value (parser, &hwb->hue, &hwb->missing[0], syntax);

    case 1:
      return parse_hwb_channel_value (parser, &hwb->white, &hwb->missing[1], syntax);

    case 2:
      return parse_hwb_channel_value (parser, &hwb->black, &hwb->missing[2], syntax);

    case 3:
      return parse_alpha_value (parser, &hwb->alpha, &hwb->missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

typedef struct {
  float L, a, b, alpha;
  gboolean missing[4];
} LabData;

static guint
parse_oklab_color_channel (GtkCssParser *parser,
                           guint         arg,
                           ColorSyntax  *syntax,
                           gpointer      data)
{
  LabData *oklab = data;

  switch (arg)
    {
    case 0:
      return parse_ok_L_value (parser, &oklab->L, &oklab->missing[0]);

    case 1:
      return parse_ok_ab_value (parser, &oklab->a, &oklab->missing[1]);

    case 2:
      return parse_ok_ab_value (parser, &oklab->b, &oklab->missing[2]);

    case 3:
      return parse_alpha_value (parser, &oklab->alpha, &oklab->missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

typedef struct {
  float L, C, H, alpha;
  gboolean missing[4];
} LchData;

static guint
parse_oklch_color_channel (GtkCssParser *parser,
                           guint         arg,
                           ColorSyntax  *syntax,
                           gpointer      data)
{
  LchData *oklch = data;

  switch (arg)
    {
    case 0:
      return parse_ok_L_value (parser, &oklch->L, &oklch->missing[0]);

    case 1:
      return parse_ok_C_value (parser, &oklch->C, &oklch->missing[1]);

    case 2:
      return parse_hue_value (parser, &oklch->H, &oklch->missing[2], syntax);

    case 3:
      return parse_alpha_value (parser, &oklch->alpha, &oklch->missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

typedef struct {
  GtkCssColorSpace color_space;
  float values[4];
  gboolean missing[4];
} ParseColorData;

static gboolean
parse_color_channel_value (GtkCssParser *parser,
                           float        *value,
                           gboolean     *missing)
{
  if (gtk_css_parser_try_ident (parser, "none"))
    {
      *missing = TRUE;
      *value = 0;
    }
  else
    {
      GtkCssNumberParseFlags flags = GTK_CSS_PARSE_NUMBER | GTK_CSS_PARSE_PERCENT;
      GtkCssValue *val;

      *missing = FALSE;

      val = gtk_css_number_value_parse (parser, flags);
      if (val == NULL)
        return FALSE;

      *value = gtk_css_number_value_get_canonical (val, 1);

      gtk_css_value_unref (val);
    }

  return TRUE;
}

static guint
parse_color_color_channel (GtkCssParser *parser,
                           guint         arg,
                           ColorSyntax  *syntax,
                           gpointer      data)
{
  ParseColorData *color_data = data;

  switch (arg)
    {
    case 0:
      if (gtk_css_parser_try_ident (parser, "srgb"))
        {
          color_data->color_space = GTK_CSS_COLOR_SPACE_SRGB;
          return 1;
        }

      if (gtk_css_parser_try_ident (parser, "srgb-linear"))
        {
          color_data->color_space = GTK_CSS_COLOR_SPACE_SRGB_LINEAR;
          return 1;
        }

      gtk_css_parser_error_syntax (parser, "Invalid color space in color()");
      return 0;

    case 1:
      return parse_color_channel_value (parser, &color_data->values[0], &color_data->missing[0]);

    case 2:
      return parse_color_channel_value (parser, &color_data->values[1], &color_data->missing[1]);

    case 3:
      return parse_color_channel_value (parser, &color_data->values[2], &color_data->missing[2]);

    case 4:
      return parse_alpha_value (parser, &color_data->values[3], &color_data->missing[3], syntax);

    default:
      g_assert_not_reached ();
    }
}

static gboolean
parse_color_function (GtkCssParser *self,
                      ColorSyntax   syntax,
                      gboolean      parse_color_space,
                      gboolean      allow_alpha,
                      gboolean      require_alpha,
                      guint (* parse_func) (GtkCssParser *, guint, ColorSyntax *, gpointer),
                      gpointer      data)
{
  const GtkCssToken *token;
  gboolean result = FALSE;
  char function_name[64];
  guint arg;
  guint min_args = 3;
  guint max_args = 4;

  if (parse_color_space)
    {
      min_args++;
      max_args++;
    }

  token = gtk_css_parser_get_token (self);
  g_return_val_if_fail (gtk_css_token_is (token, GTK_CSS_TOKEN_FUNCTION), FALSE);

  g_strlcpy (function_name, gtk_css_token_get_string (token), 64);
  gtk_css_parser_start_block (self);

  arg = 0;
  while (TRUE)
    {
      guint parse_args = parse_func (self, arg, &syntax, data);
      if (parse_args == 0)
        break;
      arg += parse_args;
      token = gtk_css_parser_get_token (self);

      if (syntax == COLOR_SYNTAX_DETECTING)
        {
          if (gtk_css_token_is (token, GTK_CSS_TOKEN_COMMA))
            {
              syntax = COLOR_SYNTAX_LEGACY;
              min_args = require_alpha ? 4 : 3;
              max_args = allow_alpha ? 4 : 3;
            }
          else
            {
              syntax = COLOR_SYNTAX_MODERN;
            }
        }

      if (gtk_css_token_is (token, GTK_CSS_TOKEN_EOF))
        {
          if (arg < min_args)
            {
              gtk_css_parser_error_syntax (self, "%s() requires at least %u arguments", function_name, min_args);
              break;
            }
          else
            {
              result = TRUE;
              break;
            }
        }
      else if (gtk_css_token_is (token, GTK_CSS_TOKEN_COMMA))
        {
          if (syntax == COLOR_SYNTAX_MODERN)
            {
              gtk_css_parser_error_syntax (self, "Commas aren't allowed in modern %s() syntax", function_name);
              break;
            }

          if (arg >= max_args)
            {
              gtk_css_parser_error_syntax (self, "Expected ')' at end of %s()", function_name);
              break;
            }

          gtk_css_parser_consume_token (self);
          continue;
        }
      else if (syntax == COLOR_SYNTAX_LEGACY)
        {
          gtk_css_parser_error_syntax (self, "Unexpected data at end of %s() argument", function_name);
          break;
        }
      else if (arg == min_args)
        {
          if (gtk_css_token_is_delim (token, '/'))
            {
              gtk_css_parser_consume_token (self);
              continue;
            }

          if (arg >= max_args)
            {
              gtk_css_parser_error_syntax (self, "Expected ')' at end of %s()", function_name);
              break;
            }

          gtk_css_parser_error_syntax (self, "Expected '/' or ')'");
          break;
        }
      else if (arg >= max_args)
        {
          gtk_css_parser_error_syntax (self, "Expected ')' at end of %s()", function_name);
          break;
        }
    }

  gtk_css_parser_end_block (self);

  return result;
}

GtkCssValue *
gtk_css_color_value_parse (GtkCssParser *parser)
{
  GtkCssValue *value;
  GdkRGBA rgba;

  if (gtk_css_parser_try_ident (parser, "currentcolor"))
    {
      return gtk_css_color_value_new_current_color ();
    }
  else if (gtk_css_parser_has_token (parser, GTK_CSS_TOKEN_AT_KEYWORD))
    {
      const GtkCssToken *token = gtk_css_parser_get_token (parser);

      gtk_css_parser_warn_deprecated (parser, "@define-color and named colors are deprecated");

      value = gtk_css_color_value_new_name (gtk_css_token_get_string (token));
      gtk_css_parser_consume_token (parser);

      return value;
    }
  else if (gtk_css_parser_has_function (parser, "rgb") || gtk_css_parser_has_function (parser, "rgba"))
    {
      gboolean has_alpha;
      ParseRGBAData data = { NULL, FALSE, { FALSE, } };
      data.rgba = &rgba;

      rgba.alpha = 1.0;

      has_alpha = gtk_css_parser_has_function (parser, "rgba");

      if (!parse_color_function (parser, COLOR_SYNTAX_DETECTING, FALSE, has_alpha, has_alpha, parse_rgba_color_channel, &data))
        return NULL;

      return gtk_css_color_value_new_color (GTK_CSS_COLOR_SPACE_SRGB, TRUE, (float *) &rgba, data.missing);
    }
  else if (gtk_css_parser_has_function (parser, "hsl") || gtk_css_parser_has_function (parser, "hsla"))
    {
      GdkHSLA hsla = { 0, 0, 0, 1 };
      ParseHSLAData data = { NULL, { FALSE, } };
      data.hsla = &hsla;

      if (!parse_color_function (parser, COLOR_SYNTAX_DETECTING, FALSE, TRUE, FALSE, parse_hsla_color_channel, &data))
        return NULL;

      return gtk_css_color_value_new_color (GTK_CSS_COLOR_SPACE_HSL, FALSE, (float *) &hsla, data.missing);
    }
  else if (gtk_css_parser_has_function (parser, "hwb"))
    {
      HwbData hwb = { 0, 0, 0, 1, { FALSE, } };

      if (!parse_color_function (parser, COLOR_SYNTAX_MODERN, FALSE, TRUE, FALSE, parse_hwb_color_channel, &hwb))
        return NULL;

      return gtk_css_color_value_new_color (GTK_CSS_COLOR_SPACE_HWB, FALSE, (float *) &hwb, hwb.missing);
    }
  else if (gtk_css_parser_has_function (parser, "oklab"))
    {
      LabData oklab = { 0, 0, 0, 1, { FALSE, } };

      if (!parse_color_function (parser, COLOR_SYNTAX_MODERN, FALSE, TRUE, FALSE, parse_oklab_color_channel, &oklab))
        return NULL;

      return gtk_css_color_value_new_color (GTK_CSS_COLOR_SPACE_OKLAB, FALSE, (float *) &oklab, oklab.missing);
    }
  else if (gtk_css_parser_has_function (parser, "oklch"))
    {
      LchData oklch = { 0, 0, 0, 1, { FALSE, } };

      if (!parse_color_function (parser, COLOR_SYNTAX_MODERN, FALSE, TRUE, FALSE, parse_oklch_color_channel, &oklch))
        return NULL;

      return gtk_css_color_value_new_color (GTK_CSS_COLOR_SPACE_OKLCH, FALSE, (float *) &oklch, oklch.missing);
    }
  else if (gtk_css_parser_has_function (parser, "color"))
    {
      ParseColorData data = { 0, { 0, 0, 0, 1 }, { FALSE, } };

      if (!parse_color_function (parser, COLOR_SYNTAX_MODERN, TRUE, TRUE, FALSE, parse_color_color_channel, &data))
        return NULL;

      return gtk_css_color_value_new_color (data.color_space, FALSE, data.values, data.missing);
    }
  else if (gtk_css_parser_has_function (parser, "lighter"))
    {
      ColorFunctionData data = { NULL, };

      if (gtk_css_parser_consume_function (parser, 1, 1, parse_color_number, &data))
        value = gtk_css_color_value_new_shade (data.color, 1.3);
      else
        value = NULL;

      g_clear_pointer (&data.color, gtk_css_value_unref);
      return value;
    }
  else if (gtk_css_parser_has_function (parser, "darker"))
    {
      ColorFunctionData data = { NULL, };

      if (gtk_css_parser_consume_function (parser, 1, 1, parse_color_number, &data))
        value = gtk_css_color_value_new_shade (data.color, 0.7);
      else
        value = NULL;

      g_clear_pointer (&data.color, gtk_css_value_unref);
      return value;
    }
  else if (gtk_css_parser_has_function (parser, "shade"))
    {
      ColorFunctionData data = { NULL, };

      if (gtk_css_parser_consume_function (parser, 2, 2, parse_color_number, &data))
        value = gtk_css_color_value_new_shade (data.color, data.value);
      else
        value = NULL;

      g_clear_pointer (&data.color, gtk_css_value_unref);
      return value;
    }
  else if (gtk_css_parser_has_function (parser, "alpha"))
    {
      ColorFunctionData data = { NULL, };

      if (gtk_css_parser_consume_function (parser, 2, 2, parse_color_number, &data))
        value = gtk_css_color_value_new_alpha (data.color, data.value);
      else
        value = NULL;

      g_clear_pointer (&data.color, gtk_css_value_unref);
      return value;
    }
  else if (gtk_css_parser_has_function (parser, "mix"))
    {
      ColorFunctionData data = { NULL, };

      if (gtk_css_parser_consume_function (parser, 3, 3, parse_color_mix, &data))
        value = gtk_css_color_value_new_mix (data.color, data.color2, data.value);
      else
        value = NULL;

      g_clear_pointer (&data.color, gtk_css_value_unref);
      g_clear_pointer (&data.color2, gtk_css_value_unref);
      return value;
    }

  if (gdk_rgba_parser_parse (parser, &rgba))
    return gtk_css_color_value_new_literal (&rgba);
  else
    return NULL;
}

const GdkRGBA *
gtk_css_color_value_get_rgba (const GtkCssValue *color)
{
  g_assert (color->class == &GTK_CSS_VALUE_COLOR);
  g_assert (color->type == COLOR_TYPE_LITERAL || color->type == COLOR_TYPE_COLOR);

  return &color->rgba;
}

const GtkCssColor *
gtk_css_color_value_get_color (const GtkCssValue *color)
{
  g_assert (color->class == &GTK_CSS_VALUE_COLOR);
  g_assert (color->type == COLOR_TYPE_COLOR);

  return &color->color;
}
