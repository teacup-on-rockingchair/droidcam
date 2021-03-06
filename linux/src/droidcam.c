/* DroidCam & DroidCamX (C) 2010-
 * Author: Aram G. (dev47@dev47apps.com)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>
#include <gtk/gtk.h>

#include <errno.h>
#include <string.h>

#include "common.h"
#include "connection.h"
#include "decoder.h"
#include "icon.h"

enum callbacks {
	CB_BUTTON = 0,
	CB_RADIO_WIFI,
	CB_RADIO_BTH,
	CB_RADIO_ADB,
	CB_WIFI_SRVR,
	CB_AUDIO,
	CB_BTN_OTR,
};

enum control_code {
	CB_CONTROL_ZIN = 16,  // 6
	CB_CONTROL_ZOUT, // 7
	CB_CONTROL_AF,  // 8
	CB_CONTROL_LED, // 9
};

#define CB_CHK_VIDEO_CTRL_CODE(cb) ((cb >= CB_CONTROL_ZIN) && (cb <= CB_CONTROL_LED))

struct settings {
	GtkEntry * ipEntry;
	GtkEntry * portEntry;
	GtkButton * button;
	int width, height;
	char connection; // Connection type
	char mirror;
	char audio;
};

/* Globals */
GtkWidget *menu;
GThread* hVideoThread;
GThread* hAudioThread;
int v_running = 0;
int a_running = 0;
int thread_cmd = 0;
int wifi_srvr_mode = 0;
struct settings g_settings = {0};

extern int m_width, m_height, m_format;

/* Helper Functions */
void ShowError(const char * title, const char * msg)
{
	if (hVideoThread != NULL)
		gdk_threads_enter();

	GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (hVideoThread != NULL)
		gdk_threads_leave();
}

static int CheckAdbDevices(int port){
	char buf[256];
	int haveDevice = 0;

	system("adb start-server");
	FILE* pipe = popen("adb devices", "r");
	if (!pipe) {
		goto _exit;
	}

	while (!feof(pipe)) {
		dbgprint("->");
		if (fgets(buf, sizeof(buf), pipe) == NULL) break;
		dbgprint("Got line: %s", buf);

		if (strstr(buf, "List of") != NULL){
			haveDevice = 2;
			continue;
		}
		if (haveDevice == 2) {
			if (strstr(buf, "offline") != NULL){
				haveDevice = 4;
				break;
			}
			if (strstr(buf, "device") != NULL && strstr(buf, "??") == NULL){
				haveDevice = 8;
				break;
			}
		}
	}
	pclose(pipe);
	#define TAIL "Please refer to the website for manual adb setup info."
	if (haveDevice == 0 || haveDevice == 1) {
		MSG_ERROR("adb.exe not detected. " TAIL);
	}
	else if (haveDevice == 2) {
		MSG_ERROR("No devices detected. " TAIL);
	}
	else if (haveDevice == 4) {
		system("adb kill-server");
		MSG_ERROR("Device is offline. Try re-attaching device.");
	}
	else if (haveDevice == 8) {
		sprintf(buf, "adb forward tcp:%d tcp:%d", port, port);
		system(buf);
	}
_exit:
	dbgprint("haveDevice = %d\n", haveDevice);
	return haveDevice;
}

static void LoadSaveSettings(int load)
{
	char buf[PATH_MAX];
	FILE * fp;

	snprintf(buf, sizeof(buf), "%s/.droidcam/settings", getenv("HOME"));
	fp = fopen(buf, (load) ? "r" : "w");

	if (load) { // Set Defaults
		g_settings.connection = CB_RADIO_WIFI;
		gtk_entry_set_text(g_settings.ipEntry, "");
		gtk_entry_set_text(g_settings.portEntry, "4747");
		g_settings.width = 320;
		g_settings.height = 240;
	}
	if (!fp){
		MSG_LASTERROR("settings error");
		return;
	}

	if (load)
	{
		if(fgets(buf, sizeof(buf), fp)){
			sscanf(buf, "%d-%d", &g_settings.width, &g_settings.height);
		}
		if(fgets(buf, sizeof(buf), fp)){
			buf[strlen(buf)-1] = '\0';
			gtk_entry_set_text(g_settings.ipEntry, buf);
		}

		if(fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf)-1] = '\0';
			gtk_entry_set_text(g_settings.portEntry, buf);
		}

	}
	else {
		fprintf(fp, "%d-%d\n%s\n%s\n", g_settings.width, g_settings.height, gtk_entry_get_text(g_settings.ipEntry), gtk_entry_get_text(g_settings.portEntry));
	}
	fclose(fp);
}

/* Audio Thread */

/* Audio Thread */
void * AudioThreadProc(void * args)
{
	char stream_buf[AUDIO_INBUF_SZ + 16]; // padded so libavcodec detects the end
	SOCKET audioSocket = (SOCKET)args;
	printf("Audio Thread Started\n");
	a_running = 1;
	
	// Send HTTP request
	strcpy(stream_buf, AUDIO_REQ);
	if ( SendRecv(1, stream_buf, sizeof(AUDIO_REQ), audioSocket) <= 0){
		MSG_ERROR("Connection lost! (Audio)");
		goto _out;
	}
	// Recieve headers
	memset(stream_buf, 0, 8);
	if ( SendRecv(0, stream_buf, 5, audioSocket) <= 0 ){
		MSG_ERROR("Connection reset (audio)!\nDroidCam is probably busy with another client.");
		goto _out;
	}
	
	dbgprint("Starting audio stream .. %s\n",stream_buf);
	memset(stream_buf, 0, sizeof(stream_buf));
	while (a_running) {
		if ( SendRecv(0, stream_buf, AUDIO_INBUF_SZ, audioSocket) == FALSE 
			|| DecodeAudio(stream_buf, AUDIO_INBUF_SZ) == FALSE) 
			break;
		dbgprint("Current audio stream .. %s\n",stream_buf);

	}
_out:
	//cleanup
	dbgprint("Audio Thread Exiting\n");
	disconnect(audioSocket);
	return NULL;
}


/* Video Thread */
void * VideoThreadProc(void * args)
{
	char stream_buf[VIDEO_INBUF_SZ + 16]; // padded so libavcodec detects the end
	SOCKET videoSocket = (SOCKET) args;
	int keep_waiting = 0;
	dbgprint("Video Thread Started s=%d\n", videoSocket);
	v_running = 1;

_wait:
	// We are the server
	if (videoSocket == INVALID_SOCKET)
	{
		videoSocket = (g_settings.connection == CB_RADIO_BTH)
			? accept_bth_connection()
			: accept_inet_connection(atoi(gtk_entry_get_text(g_settings.portEntry)));

		if (videoSocket == INVALID_SOCKET)
			goto _out;

		keep_waiting = 1;
	}

	{
		int L = sprintf(stream_buf, VIDEO_REQ, g_settings.width, g_settings.height);
		if ( SendRecv(1, stream_buf, L, videoSocket) <= 0 ){
			MSG_ERROR("Connection lost!");
			goto _out;
		}
		dbgprint("Sent request, ");
	}
	memset(stream_buf, 0, sizeof(stream_buf));

	if ( SendRecv(0, stream_buf, 5, videoSocket) <= 0 ){
		MSG_ERROR("Connection reset!\nDroidCam is probably busy with another client");
		goto _out;
	}

	if ( decoder_prepare_video(stream_buf) == FALSE )
		goto _out;

	while (v_running != 0){
		if (thread_cmd != 0) {
			int L = sprintf(stream_buf, OTHER_REQ, thread_cmd);
			SendRecv(1, stream_buf, L, videoSocket);
			thread_cmd = 0;
		}
		if ( SendRecv(0, stream_buf, VIDEO_INBUF_SZ, videoSocket) == FALSE || DecodeVideo(stream_buf, VIDEO_INBUF_SZ) == FALSE)
			break;
	}

_out:

	dbgprint("disconnect\n");
	disconnect(videoSocket);
	decoder_cleanup();

	if (v_running && keep_waiting){
		videoSocket = INVALID_SOCKET;
		goto _wait;
	}

	connection_cleanup();

	// gdk_threads_enter();
	// gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), TRUE);
	// gdk_threads_leave();
	dbgprint("Video Thread End\n");
	return NULL;
}

static void StopVideo()
{
	v_running = 0;
	if (hVideoThread != NULL)
	{
		dbgprint("Waiting for videothread..\n");
		g_thread_join(hVideoThread);
		dbgprint("videothread joined\n");
		hVideoThread = NULL;
		//gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), TRUE);
	}
}

/* Messages */
/*
static gint button_press_event(GtkWidget *widget, GdkEvent *event){
	if (event->type == GDK_BUTTON_PRESS){
		GdkEventButton *bevent = (GdkEventButton *) event;
		//if (bevent->button == 3)
		gtk_menu_popup (GTK_MENU(menu), NULL, NULL, NULL, NULL,
						bevent->button, bevent->time);
		return TRUE;
	}
	return FALSE;
}
*/


static gboolean
accel_callback( GtkAccelGroup  *group,
		  GObject		*obj,
		  guint		   keyval,
		  GdkModifierType mod,
		  gpointer		user_data)
{
	if(v_running == 1 && thread_cmd ==0 && m_format != VIDEO_FMT_H263){
		thread_cmd = (int) user_data;
	}
	return TRUE;
}


/** 
 * 
 * @brief callback for following UI commands:
 *
 * CB_CONTROL_ZIN  : zoom in
 * CB_CONTROL_ZOUT : zoom out
 * CB_CONTROL_AF   : autofocus
 * CB_CONTROL_LED  : toggle led
 *
 * 
 * @param widget 
 * @param extra 
 */
static void video_params_callback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);
	if (!CB_CHK_VIDEO_CTRL_CODE(cb))
		return;

	if(v_running == 1 && thread_cmd ==0 && m_format != VIDEO_FMT_H263){
		thread_cmd =  cb - 10;
	}
	
}



/** 
 * 
 * @brief update UI controls with labels sensitive to the type of connection we are using
 * 
 * @param buttonLabel 
 * @param ipEdit 
 * @param portEdit 
 */
static inline void DroidCamGtkUpdateConnectionsSensitiveFields(char *buttonLabel, gboolean ipEdit, gboolean portEdit)
{
	if (buttonLabel != NULL && v_running == 0){
		gtk_button_set_label(g_settings.button, buttonLabel);
		gtk_widget_set_sensitive(GTK_WIDGET(g_settings.ipEntry), ipEdit);
		gtk_widget_set_sensitive(GTK_WIDGET(g_settings.portEntry), portEdit);
	}
}

/** 
 * 
 * @brief popup menu button handler
 * 
 * @param widget 
 * @param extra 
 */
static void DroidCamPopupBtnCallback(GtkWidget *widget, gpointer extra)
{
	int cb = (int) extra;
	
	dbgprint("the_cb=%d\n", cb);
	if(cb !=  CB_BTN_OTR)
		return;
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, 0);
}


/** 
 * 
 * @brief handle audio settings control
 * 
 * @param widget 
 * @param extra 
 */
/* static */ void DroidCamAudioBtnCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);
	if(cb != CB_AUDIO)
		return;
	g_settings.audio = !g_settings.audio;
		
}

/** 
 * 
 * @brief handle radio button to enable connection via adb
 * 
 * @param widget 
 * @param extra 
 */
static void DroidCamAdbRadioCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);	 
	if(cb != CB_RADIO_ADB)
		return;

	g_settings.connection = CB_RADIO_ADB;

	DroidCamGtkUpdateConnectionsSensitiveFields("Connect", FALSE, TRUE);
	
}


/** 
 * 
 * @brief handle radio button to enable connection via bluetooth
 * 
 * @param widget 
 * @param extra 
 */
static void DroidCamBthRadioCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);	 
	if(cb != CB_RADIO_BTH)
		return;

	g_settings.connection = CB_RADIO_BTH;

	DroidCamGtkUpdateConnectionsSensitiveFields("Prepare", FALSE, FALSE);

}



/** 
 * 
 * @brief prepare UI for WIFI connection details
 * 
 */
static inline void DroidCamPrepareWifiConnectionUi()
{
	gboolean ipEdit = TRUE;
	char * text = NULL;

	g_settings.connection = CB_RADIO_WIFI;
	if (wifi_srvr_mode){
		text = "Prepare";
		ipEdit = FALSE;
	}
	else {
		text = "Connect";
	}

	DroidCamGtkUpdateConnectionsSensitiveFields(text, ipEdit, TRUE);
}

/** 
 * 
 * @brief handle radio button for wifi/lan mode
 * 
 * @param widget 
 * @param extra 
 */
static void DroidCamWifiRadioCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);
	if(cb != CB_RADIO_WIFI)
		return;

	DroidCamPrepareWifiConnectionUi();
}



/** 
 * 
 * @brief handle radio button for wifi server
 * 
 * @param widget 
 * @param extra 
 */
static void DroidCamWifiServerRadioCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);
	if(cb != CB_WIFI_SRVR)
		return;
	wifi_srvr_mode = !wifi_srvr_mode;
	if (g_settings.connection != CB_RADIO_WIFI)
		return;
	DroidCamPrepareWifiConnectionUi();

}

/** 
 * 
 * @brief update UI controls for connection parameters after we stop one connection
 * 
 * @param widget 
 * @param callback 
 */
static void DroidCamUpdateConnectionParamsUi(GtkWidget* widget, int callback)
{
	dbgprint("the_cb=%d\n", callback);
	switch(callback){
	case CB_RADIO_ADB:
		DroidCamAdbRadioCallback(widget, (gpointer)callback);
		break;
	case CB_RADIO_WIFI:
		DroidCamWifiRadioCallback(widget, (gpointer)callback);
		break;
	case CB_WIFI_SRVR:
		DroidCamWifiServerRadioCallback(widget, (gpointer)callback);
		break;
	case CB_RADIO_BTH:
		DroidCamBthRadioCallback(widget, (gpointer)callback);
		break;
	default:
		dbgprint("no handler for cb %d", callback);
		break;
	}
}

/** 
 * 
 * @brief handle connection button press when it is stopping the active connection
 *
 * @param widget 
 * 
 */
static inline void DroidCamBtnStopConnection(GtkWidget* widget)
{
	StopVideo();
	DroidCamUpdateConnectionParamsUi(widget, (int)g_settings.connection);
}

/** 
 * 
 * @brief handle press connection button to initiate connection
 * 
 */
static inline void DroidCamBtnStartConnection()
{
	char * ip = NULL;
	SOCKET vs = INVALID_SOCKET, as = INVALID_SOCKET;
	int port = atoi(gtk_entry_get_text(g_settings.portEntry));
	LoadSaveSettings(0); // Save
	
	if (g_settings.connection == CB_RADIO_ADB) {
		if (CheckAdbDevices(port) != 8) 
			return;
		ip = "127.0.0.1";
	} else if (g_settings.connection == CB_RADIO_WIFI && wifi_srvr_mode == 0) {
		ip = (char *)gtk_entry_get_text(g_settings.ipEntry);
	}
	
	if (ip != NULL) // Not Bluetooth or "Server Mode", so connect first
		{
			if (strlen(ip) < 7 || port < 1024) {
				MSG_ERROR("You must enter the correct IP address (and port) to connect to.");
				return;
			}
			gtk_button_set_label(g_settings.button, "Please wait");
			vs = connectDroidCam(ip, port);
			
			if (vs == INVALID_SOCKET){
				dbgprint("failed video connection");
				gtk_button_set_label(g_settings.button, "Connect");
				return;
			}
			
			as = connectDroidCam(ip, port);
			if (as == INVALID_SOCKET){
				dbgprint("failed audio connection");
				if(vs != INVALID_SOCKET)
					disconnect(vs);
				return;
			}
		}
	hAudioThread = g_thread_new("AudioThreadProc" , AudioThreadProc, (void*)as);
	hVideoThread = g_thread_new("VideoThreadProc" , VideoThreadProc, (void*)vs);
	gtk_button_set_label(g_settings.button, "Stop");
	//gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), FALSE);

	gtk_widget_set_sensitive(GTK_WIDGET(g_settings.ipEntry), FALSE);
	gtk_widget_set_sensitive(GTK_WIDGET(g_settings.portEntry), FALSE);
}



static void DroidCamConnectionBtnCallback(GtkWidget* widget, gpointer extra)
{
	int cb = (int) extra;

	dbgprint("the_cb=%d\n", cb);
	
	if(cb != CB_BUTTON)
		return;
	if (v_running)
		DroidCamBtnStopConnection(widget);
	else // START
		DroidCamBtnStartConnection();

	
}

/** 
 * 
 * @brief create GTK Window for the app
 * 
 *
 * @return 
 */
GtkWidget *DroidCamGtkWindow()
{
	GtkWidget *window = NULL;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "DroidCam Client");
	gtk_container_set_border_width(GTK_CONTAINER(window), 1);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_NONE);
	gtk_container_set_border_width(GTK_CONTAINER(window), 10);
//	gtk_widget_set_size_request(window, 250, 120);
	gtk_window_set_icon(GTK_WINDOW(window), gdk_pixbuf_new_from_inline(-1, icon_inline, FALSE, NULL));
	
	return window;

}


/** 
 * 
 * @brief setup accelerators for app's window
 * 
 * @param window - hook accelerators to that window
 */
void DroidCamGtkSetupAccelerators( GtkWidget *window)
{
	GtkAccelGroup *gtk_accel = gtk_accel_group_new ();
	GClosure *closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer)(CB_CONTROL_AF-10), NULL);
	gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("a"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);
	
	closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer)(CB_CONTROL_LED-10), NULL);
	gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("l"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);
	
	closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer)(CB_CONTROL_ZOUT-10), NULL);
	gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("minus"), 0, GTK_ACCEL_VISIBLE, closure);
	
	closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer)(CB_CONTROL_ZIN-10), NULL);
	gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("equal"), 0, GTK_ACCEL_VISIBLE, closure);
	
	gtk_window_add_accel_group(GTK_WINDOW(window), gtk_accel);
}

/** 
 * 
 * @brief set up app's menu
 * 
 */
void DroidCamGtkSetupMenu()
{
	GtkWidget *widget = NULL;

	menu = gtk_menu_new();

	widget = gtk_menu_item_new_with_label("DroidCamX Commands:");
	gtk_menu_append (GTK_MENU(menu), widget);
	gtk_widget_show (widget);
	gtk_widget_set_sensitive(widget, 0);

	widget = gtk_menu_item_new_with_label("Auto-Focus (Ctrl+A)");
	gtk_menu_append (GTK_MENU(menu), widget);
	gtk_widget_show (widget);
	g_signal_connect(widget, "activate", G_CALLBACK(video_params_callback), (gpointer)CB_CONTROL_AF);

	widget = gtk_menu_item_new_with_label("Toggle LED Flash (Ctrl+L)");
	gtk_menu_append (GTK_MENU(menu), widget);
	gtk_widget_show (widget);
	g_signal_connect(widget, "activate", G_CALLBACK(video_params_callback), (gpointer)CB_CONTROL_LED);

	widget = gtk_menu_item_new_with_label("Zoom In (+)");
   	gtk_menu_append (GTK_MENU(menu), widget);
	gtk_widget_show (widget);
	g_signal_connect(widget, "activate", G_CALLBACK(video_params_callback), (gpointer)CB_CONTROL_ZIN);

	widget = gtk_menu_item_new_with_label("Zoom Out (-)");
	gtk_menu_append (GTK_MENU(menu), widget);
	gtk_widget_show (widget);
	g_signal_connect(widget, "activate", G_CALLBACK(video_params_callback), (gpointer)CB_CONTROL_ZOUT);

}

void DroidCamGtkAddVideoPropertiesButton(GtkWidget *container_box)
{
	GtkWidget *button_box = NULL;
	GtkWidget *widget; // generic stuff

	button_box = gtk_hbox_new(FALSE, 1);
	widget = gtk_button_new_with_label("...");
	gtk_widget_set_size_request(widget, 40, 28);
	g_signal_connect(widget, "clicked", G_CALLBACK(DroidCamPopupBtnCallback), (gpointer)CB_BTN_OTR);
	gtk_box_pack_start(GTK_BOX(button_box), widget, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(container_box), button_box, FALSE, FALSE, 0);

}


/** 
 * 
 * @brief add controls to configre type of connection with the android device
 * 
 * @param container_box - container for the controls
 *
 * @return 
 */
void DroidCamGtkAddTypeConnCtrl(GtkWidget *container_box)
{
	GtkWidget *radio_box = NULL;
	GtkWidget *widget; // generic stuff

	radio_box = gtk_vbox_new(FALSE, 1);

	widget = gtk_radio_button_new_with_label(NULL, "WiFi / LAN");
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(widget), TRUE);
	g_signal_connect(widget, "toggled", G_CALLBACK(DroidCamWifiRadioCallback), (gpointer)CB_RADIO_WIFI);
	gtk_box_pack_start(GTK_BOX(radio_box), widget, FALSE, FALSE, 0);
	widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "Wifi Server Mode");
	g_signal_connect(widget, "toggled", G_CALLBACK(DroidCamWifiServerRadioCallback), (gpointer)CB_WIFI_SRVR);
	gtk_box_pack_start(GTK_BOX(radio_box), widget, FALSE, FALSE, 0);
	widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "Bluetooth");
	g_signal_connect(widget, "toggled", G_CALLBACK(DroidCamBthRadioCallback), (gpointer)CB_RADIO_BTH);
	gtk_box_pack_start(GTK_BOX(radio_box), widget, FALSE, FALSE, 0);
	widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "USB (over adb)");
	g_signal_connect(widget, "toggled", G_CALLBACK(DroidCamAdbRadioCallback), (gpointer)CB_RADIO_ADB);
	gtk_box_pack_start(GTK_BOX(radio_box), widget, FALSE, FALSE, 0);

	DroidCamGtkAddVideoPropertiesButton(radio_box);

	gtk_box_pack_start(GTK_BOX(container_box), radio_box, FALSE, FALSE, 0);

}


/** 
 * 
 * @brief add text field to put IP address of android device
 * 
 * @param container_box 
 */
void DroidCamGtkAddIPField(GtkWidget *container_box)
{
	GtkWidget *ip_box = NULL;
	GtkWidget *widget; // generic stuff

	ip_box = gtk_hbox_new(FALSE, 1);
	gtk_box_pack_start(GTK_BOX(ip_box), gtk_label_new("Phone IP:"), FALSE, FALSE, 0);
	widget = gtk_entry_new_with_max_length(16);
	gtk_widget_set_size_request(widget, 120, 30);
	g_settings.ipEntry = (GtkEntry*)widget;
	gtk_box_pack_start(GTK_BOX(ip_box), widget, FALSE, FALSE, 0);

	widget = gtk_alignment_new(0,0,0,0);
	gtk_container_add(GTK_CONTAINER(widget), ip_box);
	gtk_box_pack_start(GTK_BOX(container_box), widget, FALSE, FALSE, 0);

}


/** 
 * 
 * @brief add text field to put remote port on which  android device listens
 * 
 * @param container_box 
 */
void DroidCamGtkAddPortField(GtkWidget *container_box)
{
	GtkWidget *port_box = NULL;
	GtkWidget *widget; // generic stuff

	port_box = gtk_hbox_new(FALSE, 1);
	gtk_box_pack_start(GTK_BOX(port_box), gtk_label_new("DroidCam Port:"), FALSE, FALSE, 0);
	widget = gtk_entry_new_with_max_length(5);
	gtk_widget_set_size_request(widget, 60, 30);
	g_settings.portEntry = (GtkEntry*)widget;
	gtk_box_pack_start(GTK_BOX(port_box), widget, FALSE, FALSE, 0);

	widget = gtk_alignment_new(0,0,0,0);
	gtk_container_add(GTK_CONTAINER(widget), port_box);
	gtk_box_pack_start(GTK_BOX(container_box), widget, FALSE, FALSE, 0);

}


void DroidCamGtkAddConnectButton(GtkWidget *container_box)
{
	GtkWidget *button_box = NULL;
	GtkWidget *widget; // generic stuff

	button_box = gtk_hbox_new(FALSE, 1);
	widget = gtk_button_new_with_label("Connect");
	gtk_widget_set_size_request(widget, 80, 30);
	g_signal_connect(widget, "clicked", G_CALLBACK(DroidCamConnectionBtnCallback), CB_BUTTON);
	gtk_box_pack_start(GTK_BOX(button_box), widget, FALSE, FALSE, 0);
	g_settings.button = (GtkButton*)widget;

	widget = gtk_alignment_new(1,0,0,0);
	gtk_container_add(GTK_CONTAINER(widget), button_box);
	gtk_box_pack_start(GTK_BOX(container_box), widget, FALSE, FALSE, 10);

}

/** 
 * 
 * @brief add controls for the parameters for remote connection, 
 *        i.e port/ip and button to initiate the connection
 * 
 * @param container_box 
 */
void DroidCamGtkAddConnParamsCtrl(GtkWidget *container_box)
{
	GtkWidget *vbox = NULL;

	vbox = gtk_vbox_new(FALSE, 5);

	DroidCamGtkAddIPField(vbox);

	DroidCamGtkAddPortField(vbox);

	DroidCamGtkAddConnectButton(vbox);

	gtk_box_pack_start(GTK_BOX(container_box), vbox, FALSE, FALSE, 0);

}


/** 
 * 
 * @brief add window controls for 
 *
 * @param window - add controls there 
 */
void DroidCamGtkWindowAddControls(GtkWidget *window)
{
	GtkWidget *hbox= NULL;

	hbox = gtk_hbox_new(FALSE, 50);
	gtk_container_add(GTK_CONTAINER(window), hbox);

	// Toggle buttons
	DroidCamGtkAddTypeConnCtrl(hbox);

	/* TODO: Figure out audio
	widget = gtk_check_button_new_with_label("Enable Audio");
	g_signal_connect(widget, "toggled", G_CALLBACK(DroidCamAudioBtnCallback), (gpointer)CB_AUDIO);
	gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 5);
	*/

	// IP/Port/Button
	DroidCamGtkAddConnParamsCtrl(hbox);

}


int main(int argc, char *argv[])
{
	GtkWidget *window;

	// init threads
	gdk_threads_init();
	gtk_init(&argc, &argv);

	window = DroidCamGtkWindow();
	DroidCamGtkSetupAccelerators(window);
	DroidCamGtkSetupMenu();

	DroidCamGtkWindowAddControls(window);

	g_signal_connect(window, "destroy", G_CALLBACK (gtk_main_quit), NULL);
	gtk_widget_show_all(window);

	LoadSaveSettings(1); // Load
	if ( decoder_init(g_settings.width, g_settings.height) )
	{
		gdk_threads_enter();
		gtk_main();
		gdk_threads_leave();

		if (v_running == 1) StopVideo();

		decoder_fini();
		connection_cleanup();
	}

	return 0;
}
