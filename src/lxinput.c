/*
 *      lxinput.c
 *
 *      Copyright 2009-2011 PCMan <pcman.tw@gmail.com>
 *      Copyright 2009 martyj19 <martyj19@comcast.net>
 *      Copyright 2011-2013 Julien Lavergne <julien.lavergne@gmail.com>
 *      Copyright 2014 Andriy Grytsenko <andrej@rep.kiev.ua>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wordexp.h>

#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#define DEFAULT_SES "LXDE-pi"

static char* file = NULL;

static GtkWidget *dlg;
static GtkRange *mouse_accel;
static GtkRange *mouse_threshold;
static GtkRange *mouse_dclick;
static GtkToggleButton* mouse_left_handed;
static GtkRange *kb_delay;
static GtkRange *kb_interval;
static GtkToggleButton* kb_beep;
static GtkButton* kb_layout;
static GtkLabel* kb_layout_label;

static int accel = 20, old_accel = 20;
static int threshold = 10, old_threshold = 10;
static int dclick = 250, old_dclick = 250;
static gboolean left_handed = FALSE, old_left_handed = FALSE;
static float facc = 0.0, old_facc = 0.0;

static int delay = 500, old_delay = 500;
static int interval = 30, old_interval = 30;
static gboolean beep = TRUE, old_beep = TRUE;

static GList *devs = NULL;

static void reload_all_programs (void)
{
    GdkEventClient event;
    event.type = GDK_CLIENT_EVENT;
    event.send_event = TRUE;
    event.window = NULL;
    event.message_type = gdk_atom_intern("_GTK_READ_RCFILES", FALSE);
    event.data_format = 8;
    gdk_event_send_clientmessage_toall((GdkEvent *)&event);
}

static void set_dclick_time (int time)
{
    const char *session_name;
    char *user_config_file, *str, *fname;
    char cmdbuf[256];
    GKeyFile *kf;
    gsize len;

    // construct the file path
    session_name = g_getenv ("DESKTOP_SESSION");
    if (!session_name) session_name = DEFAULT_SES;
    user_config_file = g_build_filename (g_get_user_config_dir (), "lxsession/", session_name, "/desktop.conf", NULL);

    // read in data from file to a key file
    kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, user_config_file, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
    {
        g_free (user_config_file);
        return;
    }

    // update changed values in the key file
    g_key_file_set_integer (kf, "GTK", "iNet/DoubleClickTime", time);

    // write the modified key file out
    str = g_key_file_to_data (kf, &len, NULL);
    g_file_set_contents (user_config_file, str, len, NULL);

    g_free (user_config_file);
    g_free (str);

    // update the openbox double-click as well
    fname = g_strconcat (g_ascii_strdown (session_name, -1), "-rc.xml", NULL);
    user_config_file = g_build_filename (g_get_user_config_dir (), "openbox/", fname, NULL);
    g_free (fname);
    sprintf (cmdbuf, "sed -i s#'<doubleClickTime>[0-9]*</doubleClickTime>'#'<doubleClickTime>%d</doubleClickTime>'#g %s", time, user_config_file);
    system (cmdbuf);
    g_free (user_config_file);
    system ("openbox --reconfigure");

    reload_all_programs ();
}

static void on_mouse_dclick_changed (GtkRange* range, gpointer user_data)
{
    set_dclick_time ((int) gtk_range_get_value (range));
}

static void on_mouse_accel_changed(GtkRange* range, gpointer user_data)
{
    char buf[256];
    facc = gtk_range_get_value(range);
    facc = (facc / 5.0) - 1.0;

    GList *l;
    for (l = devs; l != NULL; l = l->next)
    {
        sprintf (buf, "xinput --set-prop \"pointer:%s\" \"libinput Accel Speed\" %f", l->data, facc);
        system (buf);
    }
}

static void on_mouse_threshold_changed(GtkRange* range, gpointer user_data)
{
    /* threshold = 110 - sensitivity. The lower the threshold, the higher the sensitivity */
    threshold = (int)gtk_range_get_value(range);
    XChangePointerControl(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), False, True,
                             0, 10, threshold);
}

static void on_kb_range_changed(GtkRange* range, int* val)
{
    *val = (int)gtk_range_get_value(range);
    /* apply keyboard values */
    XkbSetAutoRepeatRate(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), XkbUseCoreKbd, delay, interval);
}

/* This function is taken from Gnome's control-center 2.6.0.3 (gnome-settings-mouse.c) and was modified*/
#define DEFAULT_PTR_MAP_SIZE 128
static void set_left_handed_mouse()
{
    unsigned char *buttons;
    gint n_buttons, i;
    gint idx_1 = 0, idx_3 = 1;

    buttons = g_alloca (DEFAULT_PTR_MAP_SIZE);
    n_buttons = XGetPointerMapping (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), buttons, DEFAULT_PTR_MAP_SIZE);
    if (n_buttons > DEFAULT_PTR_MAP_SIZE)
    {
        buttons = g_alloca (n_buttons);
        n_buttons = XGetPointerMapping (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), buttons, n_buttons);
    }

    for (i = 0; i < n_buttons; i++)
    {
        if (buttons[i] == 1)
            idx_1 = i;
        else if (buttons[i] == ((n_buttons < 3) ? 2 : 3))
            idx_3 = i;
    }

    if ((left_handed && idx_1 < idx_3) ||
        (!left_handed && idx_1 > idx_3))
    {
        buttons[idx_1] = ((n_buttons < 3) ? 2 : 3);
        buttons[idx_3] = 1;
        XSetPointerMapping (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), buttons, n_buttons);
    }
}

static void on_left_handed_toggle(GtkToggleButton* btn, gpointer user_data)
{
    left_handed = gtk_toggle_button_get_active(btn);
    set_left_handed_mouse(left_handed);
}

static void on_kb_beep_toggle(GtkToggleButton* btn, gpointer user_data)
{
    XKeyboardControl values;
    beep = gtk_toggle_button_get_active(btn);
    values.bell_percent = beep ? -1 : 0;
    XChangeKeyboardControl(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), KBBellPercent, &values);
}

/*
static gboolean on_change_val(GtkRange *range, GtkScrollType scroll,
                                 gdouble value, gpointer user_data)
{
    int interval = GPOINTER_TO_INT(user_data);
    return FALSE;
}
*/

static const gchar* detect_keymap_program()
{
    const gchar *program = NULL;

    /*find if lxkeymap is in the path*/
    program = "rc_gui";
    if (g_find_program_in_path(program))
    {
        return "env SUDO_ASKPASS=/usr/bin/pwdrcg.sh sudo -A rc_gui -k";
    }
    /*TODO
    Add other keymap executable*/
    else
    {
        return NULL;
    }

}

static void on_kb_layout_clicked(GtkButton *button,  gpointer   user_data)
{

    int status;
    char* output = NULL;
    const gchar *program = detect_keymap_program();

    if (program)
    {
        g_spawn_command_line_sync(program, &output, NULL, &status, NULL );
    }

}


static void set_range_stops(GtkRange* range, int interval )
{
/*
    g_signal_connect(range, "change-value",
                    G_CALLBACK(on_change_val), GINT_TO_POINTER(interval));
*/
}

static void load_settings()
{
    const char* session_name = g_getenv("DESKTOP_SESSION");
	/* load settings from current session config files */
	if (!session_name) session_name = DEFAULT_SES;

    char* rel_path = g_strconcat("lxsession/", session_name, "/desktop.conf", NULL);
    char* user_config_file = g_build_filename(g_get_user_config_dir(), rel_path, NULL);
    GKeyFile* kf = g_key_file_new();

    if(!g_key_file_load_from_file(kf, user_config_file, G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
    {
        g_key_file_load_from_dirs(kf, rel_path, (const char**)g_get_system_config_dirs(), NULL,
                                  G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, NULL);
    }

    g_free(rel_path);

    int val;
    val = g_key_file_get_integer(kf, "Mouse", "AccFactor", NULL);
    if( val > 0)
        old_accel = accel = val;

    val = g_key_file_get_integer(kf, "Mouse", "AccThreshold", NULL);
    if( val > 0)
        old_threshold = threshold = val;

    old_left_handed = left_handed = g_key_file_get_boolean(kf, "Mouse", "LeftHanded", NULL);

    val = g_key_file_get_integer(kf, "Keyboard", "Delay", NULL);
    if(val > 0)
        old_delay = delay = val;
    val = g_key_file_get_integer(kf, "Keyboard", "Interval", NULL);
    if(val > 0)
        old_interval = interval = val;

    if( g_key_file_has_key(kf, "Keyboard", "Beep", NULL ) )
        old_beep = beep = g_key_file_get_boolean(kf, "Keyboard", "Beep", NULL);

    val = g_key_file_get_integer(kf, "GTK", "iNet/DoubleClickTime", NULL);
    if (val > 0)
        old_dclick = dclick = val;

    g_key_file_free(kf);

    g_free(user_config_file);
}

void get_valid_mice (void)
{
    FILE *fp, *fp2;
    char buf[128], *cptr, cmd[256];

    // need to get the device list from xinput first...
    fp = popen ("xinput list | grep pointer | grep slave | cut -f 1 | cut -d ' ' -f 5-", "r");
    if (fp == NULL) return;
    while (fgets (buf, sizeof (buf) - 1, fp))
    {
        cptr = buf + strlen (buf) - 1;
        while (*cptr == ' ' || *cptr == '\n') *cptr-- = 0;
        sprintf (cmd, "xinput list-props \"pointer:%s\" | grep -q \"Accel Speed\"", buf);
        fp2 = popen (cmd, "r");
        if (!pclose (fp2)) devs = g_list_append (devs, g_strdup (buf));
    }
    pclose (fp);
}

void read_mouse_speed (void)
{
    char buf[128];
    float val;
    FILE *fp = popen ("grep -Po \"Accel Speed\\\" [-.0-9]*\" ~/.config/autostart/LXinput-setup.desktop | head -1 | cut -f 3 -d ' '", "r");
    if (fp == NULL) return;
    if (fgets (buf, sizeof (buf) - 1, fp))
    {
        if (sscanf (buf, "%f", &val) == 1) facc = old_facc = val;
    }
    pclose (fp);
}

int main(int argc, char** argv)
{
    GtkBuilder* builder;
    char* str = NULL, *mstr = NULL;

    get_valid_mice ();

    GKeyFile* kf = g_key_file_new();
    const char* session_name = g_getenv("DESKTOP_SESSION");
    /* load settings from current session config files */
    if(!session_name)
        session_name = "LXDE";

    char* rel_path = g_strconcat("lxsession/", session_name, "/desktop.conf", NULL);
    char* user_config_file = g_build_filename(g_get_user_config_dir(), rel_path, NULL);

#ifdef ENABLE_NLS
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    gtk_init(&argc, &argv);

    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    /* build the UI */
    builder = gtk_builder_new();

    gtk_builder_add_from_file( builder, PACKAGE_DATA_DIR "/lxinput.ui", NULL );
    dlg = (GtkWidget*)gtk_builder_get_object( builder, "dlg" );
    gtk_dialog_set_alternative_button_order( (GtkDialog*)dlg, GTK_RESPONSE_OK, GTK_RESPONSE_CANCEL, -1 );

    mouse_accel = (GtkRange*)gtk_builder_get_object(builder,"mouse_accel");
    //mouse_threshold = (GtkRange*)gtk_builder_get_object(builder,"mouse_threshold");
    mouse_left_handed = (GtkToggleButton*)gtk_builder_get_object(builder,"left_handed");
    mouse_dclick = (GtkRange*)gtk_builder_get_object(builder, "mouse_dclick");

    kb_delay = (GtkRange*)gtk_builder_get_object(builder,"kb_delay");
    kb_interval = (GtkRange*)gtk_builder_get_object(builder,"kb_interval");
    kb_beep = (GtkToggleButton*)gtk_builder_get_object(builder,"beep");
    kb_layout = (GtkButton*)gtk_builder_get_object(builder,"keyboard_layout");

    const gchar *program = detect_keymap_program();
    if (program == NULL)
    {
        /* Hide the button if there is no program to set keymap */
        kb_layout_label = (GtkLabel*)gtk_builder_get_object(builder,"keyboard_layout_label");
        gtk_widget_set_visible(GTK_WIDGET(kb_layout_label), FALSE);
        gtk_widget_set_visible(GTK_WIDGET(kb_layout), FALSE);
    }
    else
    {
            gtk_button_set_label(kb_layout, _("Keyboard Layout..."));
    }

    g_object_unref( builder );


    /* read the config flie */
    load_settings();
    read_mouse_speed ();

    /* init the UI */
    gtk_range_set_value(mouse_accel, (facc + 1) * 5.0);
    //gtk_range_set_value(mouse_threshold, threshold);
    gtk_range_set_value(mouse_dclick, dclick);
    gtk_toggle_button_set_active(mouse_left_handed, left_handed);

    gtk_range_set_value(kb_delay, delay);
    gtk_range_set_value(kb_interval, interval);
    gtk_toggle_button_set_active(kb_beep, beep);

    set_range_stops(mouse_accel, 10);
    g_signal_connect(mouse_accel, "value-changed", G_CALLBACK(on_mouse_accel_changed), NULL);
    //set_range_stops(mouse_threshold, 10);
    //g_signal_connect(mouse_threshold, "value-changed", G_CALLBACK(on_mouse_threshold_changed), NULL);
    g_signal_connect(mouse_left_handed, "toggled", G_CALLBACK(on_left_handed_toggle), NULL);
    g_signal_connect(mouse_dclick, "value-changed", G_CALLBACK(on_mouse_dclick_changed), NULL);

    set_range_stops(kb_delay, 10);
    g_signal_connect(kb_delay, "value-changed", G_CALLBACK(on_kb_range_changed), &delay);
    set_range_stops(kb_interval, 10);
    g_signal_connect(kb_interval, "value-changed", G_CALLBACK(on_kb_range_changed), &interval);
    g_signal_connect(kb_beep, "toggled", G_CALLBACK(on_kb_beep_toggle), NULL);
    g_signal_connect(kb_layout, "clicked", G_CALLBACK(on_kb_layout_clicked), NULL);

    if( gtk_dialog_run( (GtkDialog*)dlg ) == GTK_RESPONSE_OK )
    {
        gsize len;

        if(!g_key_file_load_from_file(kf, user_config_file, G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, NULL))
        {
            /* the user config file doesn't exist, create its parent dir */
            len = strlen(user_config_file) - strlen("/desktop.conf");
            user_config_file[len] = '\0';
            g_debug("user_config_file = %s", user_config_file);
            g_mkdir_with_parents(user_config_file, 0700);
            user_config_file[len] = '/';

            g_key_file_load_from_dirs(kf, rel_path, (const char**)g_get_system_config_dirs(), NULL,
                                      G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, NULL);
        }

        g_free(rel_path);

        g_key_file_set_integer(kf, "Mouse", "AccFactor", accel);
        g_key_file_set_integer(kf, "Mouse", "AccThreshold", threshold);
        g_key_file_set_integer(kf, "Mouse", "LeftHanded", !!left_handed);

        g_key_file_set_integer(kf, "Keyboard", "Delay", delay);
        g_key_file_set_integer(kf, "Keyboard", "Interval", interval);
        g_key_file_set_integer(kf, "Keyboard", "Beep", !!beep);

        str = g_key_file_to_data(kf, &len, NULL);
        g_file_set_contents(user_config_file, str, len, NULL);
        g_free(str);

        GList *l;
        char *ndev;
        for (l = devs; l != NULL; l = l->next)
        {
            if (!mstr)
                mstr = g_strdup_printf ("; xinput --set-prop \"pointer:%s\" \"libinput Accel Speed\" %f", l->data, facc);
            else
            {
                ndev = g_strdup_printf ("; xinput --set-prop \"pointer:%s\" \"libinput Accel Speed\" %f", l->data, facc);
                mstr = g_strconcat (mstr, ndev, NULL);
                g_free (ndev);
            }
        }

        /* ask the settigns daemon to reload */
        /* FIXME: is this needed? */
        /* g_spawn_command_line_sync("lxde-settings-daemon reload", NULL, NULL, NULL, NULL); */

        /* also save settings into autostart file for non-lxsession sessions */
        g_free(user_config_file);
        rel_path = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
        user_config_file = g_build_filename(rel_path, "LXinput-setup.desktop", NULL);
        if (g_mkdir_with_parents(rel_path, 0755) == 0)
        {
            str = g_strdup_printf("[Desktop Entry]\n"
                                  "Type=Application\n"
                                  "Name=%s\n"
                                  "Comment=%s\n"
                                  "NoDisplay=true\n"
                                  "Exec=sh -c 'xset m %d/10 %d r rate %d %d b %s%s%s'\n"
                                  "NotShowIn=GNOME;KDE;XFCE;\n",
                                  _("LXInput autostart"),
                                  _("Setup keyboard and mouse using settings done in LXInput"),
                                  /* FIXME: how to setup left-handed mouse? */
                                  accel, threshold, delay, interval,
                                  beep ? "on" : "off",
                                  left_handed ? ";xmodmap -e \"pointer = 3 2 1\"" : "",
                                  mstr);
            g_file_set_contents(user_config_file, str, -1, NULL);
            g_free(str);
        }
    }
    else
    {
        /* restore to original settings */

        /* keyboard */
        delay = old_delay;
        interval = old_interval;
        beep = old_beep;
        XkbSetAutoRepeatRate(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), XkbUseCoreKbd, delay, interval);
        /* FIXME: beep? */

        /* mouse */
        accel = old_accel;
        threshold = old_threshold;
        left_handed = old_left_handed;
        //XChangePointerControl(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), True, True,
        //                         accel, 10, threshold);
        set_left_handed_mouse();
        set_dclick_time (old_dclick);

        char buf[256];
        facc = (old_facc / 5.0) - 1.0;
        GList *l;
        for (l = devs; l != NULL; l = l->next)
        {
            sprintf (buf, "xinput --set-prop \"pointer:%s\" \"libinput Accel Speed\" %f", l->data, old_facc);
            system (buf);
        }
    }

    gtk_widget_destroy( dlg );

	g_free( file );
    g_key_file_free( kf );
    g_free(user_config_file);

    return 0;
}
