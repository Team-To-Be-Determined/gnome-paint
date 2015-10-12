/***************************************************************************
 *            cv_text_tool.c
 *
 *  Sun May 23 16:37:33 2010
 *  Copyright  2010  TDB
 *  <TBD>
 ****************************************************************************/

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor Boston, MA 02110-1301,  USA
 */

#include <assert.h>
#include <gtk/gtk.h>

#include "cv_rect_select.h"
#include "file.h"
#include "undo.h"
#include "gp_point_array.h"
#include "cv_resize.h"

#include "selection.h"
#include "gp-image.h"

/* TODO dynamically re-sizeable text entry */
#define TEXT_WIDTH 200
#define TEXT_HEIGHT 100

/* The container over the canvas which the text box will be inserted into */
static GtkFixed *cv_fixed;

/*private data*/
typedef struct {
    gp_tool         tool;
    gp_canvas       *cv;
    enum {
        TEXT_WAITING,
        TEXT_PLACED
    } state;
    GtkTextView     *text_view;
    gboolean        moving;
    gint            last_x;
    gint            last_y;
} private_data;

/* GDK private stuff hack */
struct textview_impl_priv {
    GtkTextWindowType type;
    GtkWidget *widget;
    GdkWindow *window;
    GdkWindow *bin_window;
    // There is some extra data here we don't care about
};

/*Member functions*/

static gboolean button_press    ( GdkEventButton *event );
static gboolean button_release  ( GdkEventButton *event );
static gboolean button_motion   ( GdkEventMotion *event );
static void     reset           ( void );
static void     destroy         ( gpointer data  );
static void     set_cursor      ( GdkCursorType cursor_type );
static void     set_point       ( GdkPoint *p );
static void     change_cursor   ( GdkPoint *p );
/* Draw functions */
static void     draw            ( void );


static private_data     *m_priv = NULL;

static void
create_private_data( void )
{
    if (m_priv == NULL)
    {
        m_priv = g_new0 (private_data,1);
        m_priv->cv          =   NULL;
    }
}

static void
destroy_private_data( void )
{
    g_free (m_priv);
    m_priv = NULL;
}

/* Signal handler to unfuck scrolling when textview grows too big */
static gboolean
unfuck_scroll(GtkTextView *textview, ...)
{
    fprintf(stderr, "Inside unfuck_scroll handler.\n");
    if(m_priv == NULL) return FALSE;
    struct textview_impl_priv *japan =
        (struct textview_impl_priv *)m_priv->text_view->text_window;

    gdk_window_invalidate_rect(japan->bin_window, NULL, FALSE);
    gdk_window_process_updates(japan->bin_window, FALSE);
    return FALSE;
}

static void
ghetto_text_window_composite(GtkAllocation *alloc)
{
    assert(m_priv->text_view);

    gint cv_width, cv_height;
    gdk_pixmap_get_size(m_priv->cv->pixmap, &cv_width, &cv_height);

    if (alloc->x + alloc->width > cv_width) {
        alloc->width = cv_width - alloc->x;
    }
    if (alloc->y + alloc->height > cv_height) {
        alloc->height = cv_height - alloc->y;
    }

    GdkPixbuf *tmpbuf;
    GdkPixmap *tmpmap = gdk_pixmap_new(GDK_DRAWABLE(m_priv->cv->pixmap),
                                       alloc->width,
                                       alloc->height,
                                       -1);
    assert(tmpmap);

    tmpbuf = gdk_pixbuf_get_from_drawable(NULL,
                                          GDK_DRAWABLE(m_priv->cv->pixmap),
                                          NULL,
                                          alloc->x,
                                          alloc->y,
                                          0,
                                          0,
                                          alloc->width,
                                          alloc->height);
    assert(tmpbuf);
    gdk_draw_pixbuf(GDK_DRAWABLE(tmpmap),
                    NULL,
                    tmpbuf,
                    0, 0,
                    0, 0,
                    -1, -1,
                    GDK_RGB_DITHER_NONE, 0, 0);
    g_object_unref(tmpbuf);

    struct textview_impl_priv *japan =
        (struct textview_impl_priv *)m_priv->text_view->text_window;
    gdk_window_set_back_pixmap(japan->bin_window, tmpmap, FALSE);

    g_object_unref(tmpmap);
}

static void
render_to_canvas(void)
{
    gint text_x, text_y;
    gtk_container_child_get(GTK_CONTAINER(cv_fixed),
                            GTK_WIDGET(m_priv->text_view),
                            "x",
                            &text_x,
                            "y",
                            &text_y,
                            NULL);
    gtk_widget_queue_draw ( m_priv->cv->widget );
    struct textview_impl_priv *japan =
        (struct textview_impl_priv *)m_priv->text_view->text_window;
    gdk_window_redirect_to_drawable(japan->bin_window,
                                    m_priv->cv->pixmap,
                                    0,
                                    0,
                                    text_x,
                                    text_y,
                                    -1,
                                    -1);
    gtk_text_view_set_cursor_visible(m_priv->text_view, FALSE);
    unfuck_scroll(NULL);

    gtk_widget_destroy(GTK_WIDGET(m_priv->text_view));
    m_priv->text_view = NULL;
    gtk_widget_queue_draw(m_priv->cv->widget);
    gdk_window_process_updates(gtk_widget_get_parent_window(m_priv->cv->widget),
                               FALSE);
}

#if 0
/* XXX shelved for now ... */
gboolean
on_text_button_press_event (	GtkWidget	   *widget, 
                                GdkEventButton *event,
                                gpointer       user_data )
{
    fprintf(stderr, "whoop3\n");
    if (event->type == GDK_BUTTON_PRESS && event->button == RIGHT_BUTTON) {
        fprintf(stderr, "whoop\n");
        m_priv->moving = TRUE;
        m_priv->last_x = (gint)event->x;
        m_priv->last_y = (gint)event->y;
    }

    return TRUE;
}

gboolean
on_text_button_release_event (	GtkWidget	   *widget, 
                                GdkEventButton *event,
                                gpointer       user_data )
{
    fprintf(stderr, "whoop4\n");
	if (event->type == GDK_BUTTON_RELEASE && event->button == RIGHT_BUTTON) {
        m_priv->moving = FALSE;
    }

	return TRUE;
}
									
gboolean
on_text_motion_notify_event (	GtkWidget      *widget,
                                GdkEventMotion *event,
                                gpointer        user_data)
{
    fprintf(stderr, "whoop2 %f %f\n", event->x, event->y);
	if (m_priv->moving) {
        GtkAllocation tv_alloc = widget->allocation;
        tv_alloc.x = event->x;
        tv_alloc.y = event->y;
        gtk_fixed_move(cv_fixed,
                       widget,
                       event->x,
                       event->y);
        ghetto_text_window_composite(&tv_alloc);
    }

    return TRUE;
}
#endif

gp_tool *
tool_text_init ( gp_canvas * canvas )
{
    create_private_data();
    m_priv->cv                  = canvas;
    m_priv->tool.button_press   = button_press;
    m_priv->tool.button_release = button_release;
    m_priv->tool.button_motion  = button_motion;
    m_priv->tool.draw           = draw;
    m_priv->tool.reset          = reset;
    m_priv->tool.destroy        = destroy;

    m_priv->state               = TEXT_WAITING;
    m_priv->text_view           = NULL;

    return &m_priv->tool;
}

void
on_cv_fixed_realize(GtkWidget *fixed, gpointer user_data)
{
    cv_fixed = GTK_FIXED(fixed);
}

static gboolean
button_press ( GdkEventButton *event )
{
    fprintf(stderr, "boop\n");
    if (event->type == GDK_BUTTON_PRESS) {
        switch (m_priv->state) {
        case TEXT_WAITING: {
            assert(!m_priv->text_view);
            m_priv->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
            m_priv->tool.no_resize = TRUE;
            m_priv->state = TEXT_PLACED;

            GtkAllocation tv_alloc;
            gint x = (gint)event->x, y = (gint)event->y;

            gint cv_width = GTK_WIDGET(cv_fixed)->allocation.width,
                 cv_height = GTK_WIDGET(cv_fixed)->allocation.height;

            tv_alloc = (GtkAllocation) {
                .x = x,
                .y = y,
                .width = x + TEXT_WIDTH > cv_width
                            ? cv_width - x
                            : TEXT_WIDTH,
                .height = y + TEXT_HEIGHT > cv_height
                            ? cv_height - y
                            : TEXT_HEIGHT
            };

            gtk_fixed_put(cv_fixed, GTK_WIDGET(m_priv->text_view), x, y);
            gtk_widget_set_size_request(GTK_WIDGET(m_priv->text_view),
                                        tv_alloc.width,
                                        tv_alloc.height);
            gtk_text_view_set_wrap_mode(m_priv->text_view, GTK_WRAP_CHAR);
            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(m_priv->text_view),
                                             TRUE);

            ghetto_text_window_composite(&tv_alloc);

            /* XXX
            g_signal_connect_after (m_priv->text_view, "popup-menu",
                              G_CALLBACK (gtk_true),
                              NULL);
            g_signal_connect_after (m_priv->text_view, "populate-popup",
                              G_CALLBACK (gtk_true),
                              NULL);
            g_signal_connect(m_priv->text_view, "button-press-event",
                                   G_CALLBACK (on_text_button_press_event), NULL);
            g_signal_connect(m_priv->text_view, "button-release-event",
                                   G_CALLBACK (on_text_button_release_event), NULL);
            g_signal_connect(m_priv->text_view, "motion-notify-event",
                                   G_CALLBACK (on_text_motion_notify_event), NULL);
            XXX */
            g_signal_connect_after (m_priv->text_view, "event",
                                    G_CALLBACK (unfuck_scroll), NULL);

            gtk_widget_show(GTK_WIDGET(m_priv->text_view));
            gtk_widget_grab_focus(GTK_WIDGET(m_priv->text_view));
            } break;
        case TEXT_PLACED: {
            /* if we received this, it means the click was outside the box - we
             * should therefore draw the current text to the canvas and
             * re-initialize */
            render_to_canvas();
            m_priv->state = TEXT_WAITING;
            } break;
        }
    }

    return TRUE;
}


static gboolean
button_release ( GdkEventButton *event )
{
    fprintf(stderr, "beep\n");
    // TODO: This was from rect_select. What do we even do here?
    /* if ( event->type == GDK_BUTTON_RELEASE )
    {
        if ( m_priv->state == SEL_DRAWING )
        {
            GdkPoint p;
            p.x = (gint)event->x;
            p.y = (gint)event->y;
            set_point ( &p );
        }
        m_priv->state = SEL_WAITING;
        gp_selection_set_borders ( TRUE );
        gtk_widget_queue_draw ( m_priv->cv->widget );
    } */
    return TRUE;
}

static gboolean
button_motion ( GdkEventMotion *event )
{
    // TODO: This was from rect_select. What do we even do here?
    /*
    GdkPoint p;
    p.x = (gint)event->x;
    p.y = (gint)event->y;
    switch ( m_priv->state )
    {
        case SEL_DRAWING:
        {
            set_point ( &p );
            gtk_widget_queue_draw ( m_priv->cv->widget );
            break;
        }
        case SEL_WAITING:
        {
            change_cursor ( &p );
            break;
        }
        case SEL_ACTION:
        {
            gp_selection_do_action ( &p );
            gtk_widget_queue_draw ( m_priv->cv->widget );
            break;
        }
    } */
    return TRUE;
}


static void
draw ( void )
{
    // TODO: This was from rect_select. What do we even do here?
    /* gp_selection_draw (NULL); */
}

static void
reset ( void )
{
    set_cursor ( GDK_XTERM );
}

static void
destroy ( gpointer data  )
{
    g_print("text tool destroy\n");

    if (m_priv->state == TEXT_PLACED) {
        render_to_canvas();
        gtk_widget_destroy(GTK_WIDGET(m_priv->text_view));
    }

    destroy_private_data();
}

static void
set_cursor ( GdkCursorType cursor_type )
{
    static GdkCursorType last_cursor = GDK_LAST_CURSOR;
    if ( cursor_type != last_cursor )
    {
        GdkCursor *cursor = gdk_cursor_new ( cursor_type );
        g_assert(cursor);
        gdk_window_set_cursor ( m_priv->cv->drawing, cursor );
        gdk_cursor_unref( cursor );
        last_cursor = cursor_type;
    }
}

static void
set_point ( GdkPoint *p )
{
    // TODO: This was from rect_select. What do we even do here?
    /*
    gint x1,y1;
    GdkRectangle rect;
    cv_get_rect_size ( &rect);
    x1 = rect.width - 1;
    y1 = rect.height - 1;
    if ( p->x < 0 ) p->x = 0;
    else if ( p->x > x1 ) p->x = x1;
    if ( p->y < 0 ) p->y = 0;
    else if ( p->y > y1 ) p->y = y1;
    gp_selection_clipbox_set_end_point ( p );
    */
}

static void
change_cursor ( GdkPoint *p )
{
    GdkCursorType cursor;
    cursor = gp_selection_get_cursor ( p );
    if ( cursor == GDK_BLANK_CURSOR )
    {
        set_cursor ( GDK_XTERM );
    }
    else
    {
        set_cursor ( cursor );
    }
}



