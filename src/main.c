#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "agent_tools.h"
#include "mcp_client.h"

typedef struct {
    gchar *role;
    gchar *text;
} Message;

typedef struct {
    gchar *title;
    gchar *system_prompt;
    gchar *agent_id;
    GPtrArray *messages;
    gboolean temporary;
    gboolean agent_mode;
    guint permissions;
} Chat;

enum {
    PERMISSION_READ = 1 << 0,
    PERMISSION_WRITE = 1 << 1,
    PERMISSION_MCP = 1 << 2
};

typedef struct {
    gchar *id, *name, *description, *system_prompt, *provider, *model;
} Agent;

typedef struct {
    GtkWidget *window, *chat_list, *transcript, *input, *send, *status, *paned;
    GtkWidget *notebook, *provider_combo, *model_entry, *attachments_list;
    GtkWidget *attachment_count, *agent_label, *project_label, *mcp_label, *agent_empty, *cancel;
    GtkWidget *mode_combo, *token_label, *permission_label, *agent_context;
    GtkWidget *left_panel, *right_panel;
    GtkListStore *log_store;
    GtkListStore *changes_store;
    GtkWidget *changes_view;
    GHashTable *changes;
    GtkListStore *agent_store;
    GPtrArray *chats;
    GPtrArray *agents;
    GPtrArray *attachments;
    McpManager *mcp;
    Chat *current;
    gchar *config_dir, *history_path, *settings_path, *agents_path;
    gchar *project_root;
    gchar *provider, *openai_url, *openai_key, *openai_model;
    gchar *ollama_url, *ollama_model;
    gchar *emoji_font;
    gchar *user_color, *assistant_color;
    gint window_width, window_height, paned_position;
    gboolean left_panel_visible, right_panel_visible;
    guint autosave_id;
    gboolean emoji_font_checked, emoji_font_available;
    gboolean busy;
    gint cancel_requested;
    gint allow_read; /* Legacy initialization slot; no longer controls tools. */
} App;

typedef struct { char *data; size_t len; } Buffer;
typedef struct {
    App *app;
    Chat *chat;
    Message *answer;
    gchar *request, *url, *key;
    gboolean openai;
} Job;
typedef struct {
    App *app;
    Chat *chat;
    Message *answer;
    gchar *delta;
    gchar *error;
    gboolean done;
} StreamEvent;
static Agent *find_agent(App *a,const gchar *id);

static void log_event(App *a, const gchar *action, const gchar *state) {
    GtkTreeIter iter;
    GDateTime *now;
    gchar *time;
    if(!a->log_store)return;
    now=g_date_time_new_now_local();
    time=g_date_time_format(now,"%H:%M:%S");
    gtk_list_store_append(a->log_store,&iter);
    gtk_list_store_set(a->log_store,&iter,0,time,1,action,2,state,3,"—",-1);
    g_free(time);
    g_date_time_unref(now);
}
typedef struct { App *app; gchar *action; gchar *state; } LogEvent;
static gboolean deliver_log_event(gpointer data) {
    LogEvent *event=data;
    log_event(event->app,event->action,event->state);
    g_free(event->action);g_free(event->state);g_free(event);
    return FALSE;
}
static void queue_log_event(App *a,const gchar *action,const gchar *state) {
    LogEvent *event=g_new0(LogEvent,1);
    event->app=a;event->action=g_strdup(action);event->state=g_strdup(state);
    g_idle_add(deliver_log_event,event);
}
typedef struct { App *app; gchar *path; gchar *diff; } DiffEvent;
static gboolean deliver_diff_event(gpointer data){
    DiffEvent *event=data;GtkTreeIter iter;GtkTreeModel *model;gboolean found=FALSE;
    g_hash_table_replace(event->app->changes,g_strdup(event->path),g_strdup(event->diff));
    model=GTK_TREE_MODEL(event->app->changes_store);
    if(gtk_tree_model_get_iter_first(model,&iter)){
        do{gchar*path=NULL;gtk_tree_model_get(model,&iter,0,&path,-1);found=!strcmp(path,event->path);g_free(path);if(found)break;}while(gtk_tree_model_iter_next(model,&iter));
    }
    if(!found){gtk_list_store_append(event->app->changes_store,&iter);gtk_list_store_set(event->app->changes_store,&iter,0,event->path,-1);}
    if(gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(event->app->changes_view)))==0)
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(event->app->changes_view)),event->diff,-1);
    g_free(event->path);g_free(event->diff);g_free(event);return FALSE;
}
static void queue_diff_event(App*a,const gchar*path,gchar*diff){
    DiffEvent*event;if(!diff||!*diff){g_free(diff);return;}event=g_new0(DiffEvent,1);
    event->app=a;event->path=g_strdup(path);event->diff=diff;g_idle_add(deliver_diff_event,event);
}

static void message_free(gpointer p) {
    Message *m = p; if (!m) return; g_free(m->role); g_free(m->text); g_free(m);
}
static void agent_free(gpointer p){
    Agent*a=p;if(!a)return;g_free(a->id);g_free(a->name);g_free(a->description);
    g_free(a->system_prompt);g_free(a->provider);g_free(a->model);g_free(a);
}
static Chat *chat_new(gboolean temporary) {
    Chat *c = g_new0(Chat, 1); c->title = g_strdup(temporary ? "Временный чат" : "Новый чат");
    c->system_prompt = g_strdup("Ты полезный ассистент.");
    c->messages = g_ptr_array_new_with_free_func(message_free); c->temporary = temporary;
    c->agent_mode = TRUE;
    return c;
}
static void chat_free(gpointer p) {
    Chat *c = p; if (!c) return; g_free(c->title); g_free(c->system_prompt);g_free(c->agent_id);
    g_ptr_array_free(c->messages, TRUE); g_free(c);
}
static void add_message(Chat *c, const char *role, const char *text) {
    Message *m = g_new0(Message, 1); m->role = g_strdup(role); m->text = g_strdup(text);
    g_ptr_array_add(c->messages, m);
}

static void set_str(gchar **dst, const gchar *src) { g_free(*dst); *dst = g_strdup(src ? src : ""); }
static gchar *config_file(App *a, const char *name) { return g_build_filename(a->config_dir, name, NULL); }

static void load_settings(App *a) {
    GKeyFile *k = g_key_file_new(); GError *e = NULL;
    a->provider = g_strdup("ollama"); a->openai_url = g_strdup("https://api.openai.com/v1/chat/completions");
    a->openai_model = g_strdup("gpt-4o-mini"); a->openai_key = g_strdup(g_getenv("OPENAI_API_KEY"));
    a->ollama_url = g_strdup("http://127.0.0.1:11434/api/chat"); a->ollama_model = g_strdup("gemma3:1b");
    a->emoji_font = g_strdup("Noto Emoji");
    a->user_color = g_strdup("#cc0000"); a->assistant_color = g_strdup("#000000");
    a->window_width = 900; a->window_height = 560; a->paned_position = 190;
    a->left_panel_visible=TRUE;a->right_panel_visible=TRUE;
    if (g_key_file_load_from_file(k, a->settings_path, G_KEY_FILE_NONE, &e)) {
#define GET(KEY, FIELD) do { gchar *v = g_key_file_get_string(k,"main",KEY,NULL); if(v){set_str(&a->FIELD,v);g_free(v);} } while(0)
        GET("provider", provider); GET("openai_url", openai_url); GET("openai_model", openai_model);
        if (!g_getenv("OPENAI_API_KEY")) GET("openai_key", openai_key);
        GET("ollama_url", ollama_url); GET("ollama_model", ollama_model);
        GET("project_root", project_root);
        GET("emoji_font", emoji_font);
        GET("user_color", user_color); GET("assistant_color", assistant_color);
#undef GET
        a->window_width = g_key_file_get_integer(k,"window","width",NULL);
        a->window_height = g_key_file_get_integer(k,"window","height",NULL);
        a->paned_position = g_key_file_get_integer(k,"window","paned_position",NULL);
        if(g_key_file_has_key(k,"window","left_panel_visible",NULL))a->left_panel_visible=g_key_file_get_boolean(k,"window","left_panel_visible",NULL);
        if(g_key_file_has_key(k,"window","right_panel_visible",NULL))a->right_panel_visible=g_key_file_get_boolean(k,"window","right_panel_visible",NULL);
        if(a->window_width<480)a->window_width=900;
        if(a->window_height<320)a->window_height=560;
        if(a->paned_position<100)a->paned_position=190;
    }
    if (e)
        g_error_free(e);
    g_key_file_free(k);
}
static void save_settings(App *a) {
    GKeyFile *k = g_key_file_new(); gsize n; gchar *data;
    g_key_file_set_string(k,"main","provider",a->provider); g_key_file_set_string(k,"main","openai_url",a->openai_url);
    g_key_file_set_string(k,"main","openai_model",a->openai_model); g_key_file_set_string(k,"main","openai_key",g_getenv("OPENAI_API_KEY")?"":a->openai_key);
    g_key_file_set_string(k,"main","ollama_url",a->ollama_url); g_key_file_set_string(k,"main","ollama_model",a->ollama_model);
    g_key_file_set_string(k,"main","project_root",a->project_root);
    g_key_file_set_string(k,"main","emoji_font",a->emoji_font);
    g_key_file_set_string(k,"main","user_color",a->user_color);g_key_file_set_string(k,"main","assistant_color",a->assistant_color);
    g_key_file_set_integer(k,"window","width",a->window_width);g_key_file_set_integer(k,"window","height",a->window_height);
    g_key_file_set_integer(k,"window","paned_position",a->paned_position);
    g_key_file_set_boolean(k,"window","left_panel_visible",a->left_panel_visible);
    g_key_file_set_boolean(k,"window","right_panel_visible",a->right_panel_visible);
    data = g_key_file_to_data(k,&n,NULL);
    gchar *tmp=g_strconcat(a->settings_path,".tmp",NULL);
    if(g_file_set_contents(tmp,data,n,NULL)&&g_rename(tmp,a->settings_path)==0)chmod(a->settings_path,0600);
    else g_unlink(tmp);
    g_free(tmp);
    g_free(data); g_key_file_free(k);
}

static void save_history(App *a) {
    json_object *root = json_object_new_array(); guint i,j;
    for (i=0;i<a->chats->len;i++) { Chat *c=g_ptr_array_index(a->chats,i); if(c->temporary) continue;
        json_object *o=json_object_new_object(), *msgs=json_object_new_array();
        json_object_object_add(o,"title",json_object_new_string(c->title));
        json_object_object_add(o,"system",json_object_new_string(c->system_prompt));
        json_object_object_add(o,"agent_id",json_object_new_string(c->agent_id?c->agent_id:""));
        json_object_object_add(o,"mode",json_object_new_string(c->agent_mode?"agent":"dialog"));
        json_object_object_add(o,"permissions",json_object_new_int64(c->permissions));
        for(j=0;j<c->messages->len;j++){ Message *m=g_ptr_array_index(c->messages,j); json_object *mo=json_object_new_object();
            json_object_object_add(mo,"role",json_object_new_string(m->role)); json_object_object_add(mo,"content",json_object_new_string(m->text)); json_object_array_add(msgs,mo); }
        json_object_object_add(o,"messages",msgs); json_object_array_add(root,o);
    }
    const gchar *data=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN);
    gchar *tmp=g_strconcat(a->history_path,".tmp",NULL);
    if(g_file_set_contents(tmp,data,-1,NULL)&&g_rename(tmp,a->history_path)!=0)g_unlink(tmp);
    g_free(tmp);json_object_put(root);
}
static void load_history(App *a) {
    json_object *root=json_object_from_file(a->history_path); size_t i,j;
    if(!root || !json_object_is_type(root,json_type_array)){ if(root)json_object_put(root); return; }
    for(i=0;i<json_object_array_length(root);i++){ json_object *o=json_object_array_get_idx(root,i), *v, *msgs; Chat *c=chat_new(FALSE);
        if(json_object_object_get_ex(o,"title",&v)) set_str(&c->title,json_object_get_string(v));
        if(json_object_object_get_ex(o,"system",&v)) set_str(&c->system_prompt,json_object_get_string(v));
        if(json_object_object_get_ex(o,"agent_id",&v)) set_str(&c->agent_id,json_object_get_string(v));
        if(json_object_object_get_ex(o,"mode",&v)) c->agent_mode=!strcmp(json_object_get_string(v),"agent");
        if(json_object_object_get_ex(o,"permissions",&v)) c->permissions=(guint)json_object_get_int64(v);
        if(json_object_object_get_ex(o,"messages",&msgs)) for(j=0;j<json_object_array_length(msgs);j++){ json_object *mo=json_object_array_get_idx(msgs,j),*r,*t;
            if(json_object_object_get_ex(mo,"role",&r)&&json_object_object_get_ex(mo,"content",&t)) add_message(c,json_object_get_string(r),json_object_get_string(t)); }
        g_ptr_array_add(a->chats,c);
    } json_object_put(root);
}

static void save_agents(App*a){
    json_object*root=json_object_new_array();guint i;gchar*tmp;
    for(i=0;i<a->agents->len;i++){Agent*agent=g_ptr_array_index(a->agents,i);json_object*o=json_object_new_object();
        json_object_object_add(o,"id",json_object_new_string(agent->id));json_object_object_add(o,"name",json_object_new_string(agent->name));
        json_object_object_add(o,"description",json_object_new_string(agent->description));json_object_object_add(o,"system_prompt",json_object_new_string(agent->system_prompt));
        json_object_object_add(o,"provider",json_object_new_string(agent->provider));json_object_object_add(o,"model",json_object_new_string(agent->model));json_object_array_add(root,o);}
    tmp=g_strconcat(a->agents_path,".tmp",NULL);
    if(g_file_set_contents(tmp,json_object_to_json_string_ext(root,JSON_C_TO_STRING_PRETTY),-1,NULL)&&g_rename(tmp,a->agents_path)!=0)g_unlink(tmp);
    g_free(tmp);json_object_put(root);
}
static void load_agents(App*a){
    json_object*root=json_object_from_file(a->agents_path);guint i;
    if(!root||!json_object_is_type(root,json_type_array)){if(root)json_object_put(root);return;}
    for(i=0;i<json_object_array_length(root);i++){json_object*o=json_object_array_get_idx(root,i),*v;Agent*agent=g_new0(Agent,1);
#define AGENT_GET(KEY,FIELD) if(json_object_object_get_ex(o,KEY,&v))agent->FIELD=g_strdup(json_object_get_string(v));else agent->FIELD=g_strdup("")
        AGENT_GET("id",id);AGENT_GET("name",name);AGENT_GET("description",description);AGENT_GET("system_prompt",system_prompt);AGENT_GET("provider",provider);AGENT_GET("model",model);
#undef AGENT_GET
        if(*agent->id&&*agent->name)g_ptr_array_add(a->agents,agent);else agent_free(agent);
    }json_object_put(root);
}

static gboolean autosave_now(gpointer data) {
    App *a = data;
    a->autosave_id = 0;
    save_history(a);
    save_settings(a);
    return FALSE;
}

static void schedule_autosave(App *a) {
    if (a->autosave_id)
        g_source_remove(a->autosave_id);
    a->autosave_id = g_timeout_add(750, autosave_now, a);
}

static gboolean is_emoji(gunichar c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) ||
           (c >= 0x2600 && c <= 0x27BF) ||
           c == 0x200D || c == 0xFE0F;
}

static gboolean font_is_installed(GtkWidget *widget, const gchar *name) {
    PangoFontFamily **families = NULL;
    gint count = 0, i;
    gboolean found = FALSE;
    pango_context_list_families(gtk_widget_get_pango_context(widget), &families, &count);
    for (i = 0; i < count; i++) {
        if (!g_ascii_strcasecmp(pango_font_family_get_name(families[i]), name)) {
            found = TRUE;
            break;
        }
    }
    g_free(families);
    return found;
}

static void apply_emoji_font(App *a, GtkTextBuffer *buffer, const gchar *text) {
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, "emoji");
    const gchar *p = text;
    gint offset = 0;

    if (!a->emoji_font_checked) {
        a->emoji_font_available = font_is_installed(a->transcript, a->emoji_font);
        a->emoji_font_checked = TRUE;
    }
    if (!a->emoji_font_available)
        return;
    if (!tag)
        tag = gtk_text_buffer_create_tag(buffer, "emoji", "family", a->emoji_font, NULL);
    else
        g_object_set(tag, "family", a->emoji_font, NULL);
    while (*p) {
        const gchar *next = g_utf8_next_char(p);
        if (is_emoji(g_utf8_get_char(p))) {
            GtkTextIter start, end;
            gtk_text_buffer_get_iter_at_offset(buffer, &start, offset);
            gtk_text_buffer_get_iter_at_offset(buffer, &end, offset + 1);
            gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
        }
        p = next;
        offset++;
    }
}

static GtkTextTag *text_tag(GtkTextBuffer *buffer, const gchar *name) {
    return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buffer), name);
}

static void ensure_chat_tags(App *a, GtkTextBuffer *buffer) {
    GtkTextTag *tag;
    tag=text_tag(buffer,"user");
    if(!tag)tag=gtk_text_buffer_create_tag(buffer,"user","foreground",a->user_color,NULL);
    else g_object_set(tag,"foreground",a->user_color,NULL);
    tag=text_tag(buffer,"assistant");
    if(!tag)tag=gtk_text_buffer_create_tag(buffer,"assistant","foreground",a->assistant_color,NULL);
    else g_object_set(tag,"foreground",a->assistant_color,NULL);
    if(!text_tag(buffer,"bold"))gtk_text_buffer_create_tag(buffer,"bold","weight",PANGO_WEIGHT_BOLD,NULL);
    if(!text_tag(buffer,"italic"))gtk_text_buffer_create_tag(buffer,"italic","style",PANGO_STYLE_ITALIC,NULL);
    if(!text_tag(buffer,"code"))gtk_text_buffer_create_tag(buffer,"code","family","monospace","background","#eeeeee",NULL);
    if(!text_tag(buffer,"codeblock"))gtk_text_buffer_create_tag(buffer,"codeblock","family","monospace","background","#eeeeee","left-margin",12,"right-margin",8,NULL);
    if(!text_tag(buffer,"heading"))gtk_text_buffer_create_tag(buffer,"heading","weight",PANGO_WEIGHT_BOLD,"scale",1.25,NULL);
    if(!text_tag(buffer,"quote"))gtk_text_buffer_create_tag(buffer,"quote","style",PANGO_STYLE_ITALIC,"left-margin",18,NULL);
    if(!text_tag(buffer,"link"))gtk_text_buffer_create_tag(buffer,"link","foreground","#3465a4","underline",PANGO_UNDERLINE_SINGLE,NULL);
    if(!text_tag(buffer,"rule"))gtk_text_buffer_create_tag(buffer,"rule","scale",0.85,NULL);
    if(!text_tag(buffer,"table"))gtk_text_buffer_create_tag(buffer,"table","family","monospace","background","#f6f6f6","left-margin",8,"right-margin",8,NULL);
}

static void insert_tagged(GtkTextBuffer *buffer, const gchar *text, gssize length, GtkTextTag *tag) {
    GtkTextIter start,end;
    gint start_offset=gtk_text_buffer_get_char_count(buffer);
    gtk_text_buffer_get_end_iter(buffer,&end);
    gtk_text_buffer_insert(buffer,&end,text,length);
    if(tag){gtk_text_buffer_get_iter_at_offset(buffer,&start,start_offset);gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_apply_tag(buffer,tag,&start,&end);}
}

static void insert_inline_markdown(GtkTextBuffer *buffer, const gchar *text) {
    const gchar *p=text;
    while(*p){
        const gchar *end=NULL;
        GtkTextTag *tag=NULL;
        gint opening=0,closing=0;
        if(g_str_has_prefix(p,"**")&&(end=strstr(p+2,"**"))){tag=text_tag(buffer,"bold");opening=2;closing=2;}
        else if(*p=='*'&&(end=strchr(p+1,'*'))){tag=text_tag(buffer,"italic");opening=1;closing=1;}
        else if(*p=='`'&&(end=strchr(p+1,'`'))){tag=text_tag(buffer,"code");opening=1;closing=1;}
        else if(*p=='['){
            const gchar *label_end=strchr(p+1,']');
            if(label_end&&label_end[1]=='('&&(end=strchr(label_end+2,')'))){
                insert_tagged(buffer,p+1,label_end-(p+1),text_tag(buffer,"link"));
                insert_tagged(buffer," (",-1,NULL);insert_tagged(buffer,label_end+2,end-(label_end+2),text_tag(buffer,"link"));insert_tagged(buffer,")",-1,NULL);
                p=end+1;continue;
            }
        }
        if(end){insert_tagged(buffer,p+opening,end-(p+opening),tag);p=end+closing;continue;}
        end=p+1;
        while(*end&&*end!='*'&&*end!='`'&&*end!='[')end++;
        insert_tagged(buffer,p,end-p,NULL);p=end;
    }
}

static gboolean is_markdown_rule(const gchar *line) {
    gchar marker=0;
    guint count=0;
    while(*line){
        if(g_ascii_isspace(*line)){line++;continue;}
        if(!marker){if(*line!='-'&&*line!='*'&&*line!='_')return FALSE;marker=*line;}
        if(*line!=marker)return FALSE;
        count++;line++;
    }
    return count>=3;
}

static GPtrArray *markdown_table_cells(const gchar *line) {
    GPtrArray *cells=g_ptr_array_new_with_free_func(g_free);
    gchar **parts=g_strsplit(line,"|",-1);
    guint i,start=0,end;
    while(parts[start]&&!*g_strstrip(parts[start]))start++;
    end=g_strv_length(parts);
    while(end>start&&!*g_strstrip(parts[end-1]))end--;
    for(i=start;i<end;i++)g_ptr_array_add(cells,g_strdup(g_strstrip(parts[i])));
    g_strfreev(parts);
    return cells;
}

static gboolean is_markdown_table_separator(const gchar *line) {
    GPtrArray *cells;
    guint i;
    gboolean valid=TRUE;
    if(!strchr(line,'|'))return FALSE;
    cells=markdown_table_cells(line);
    if(!cells->len)valid=FALSE;
    for(i=0;i<cells->len&&valid;i++){
        const gchar *p=g_ptr_array_index(cells,i);
        guint dashes=0;
        if(*p==':')p++;
        while(*p=='-'){dashes++;p++;}
        if(*p==':')p++;
        while(g_ascii_isspace(*p))p++;
        if(dashes<3||*p)valid=FALSE;
    }
    g_ptr_array_free(cells,TRUE);
    return valid;
}

static void append_table_border(GString *out,const guint *widths,guint columns) {
    guint column,i;
    g_string_append_c(out,'+');
    for(column=0;column<columns;column++){
        for(i=0;i<widths[column]+2;i++)g_string_append_c(out,'-');
        g_string_append_c(out,'+');
    }
    g_string_append_c(out,'\n');
}

static guint insert_markdown_table(GtkTextBuffer *buffer,gchar **lines,guint first) {
    GPtrArray *rows=g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    GPtrArray *header=markdown_table_cells(lines[first]);
    guint columns=header->len,*widths,i,j,last=first+2;
    GString *out;
    g_ptr_array_add(rows,header);
    while(lines[last]&&strchr(lines[last],'|')&&*g_strstrip(lines[last])){
        GPtrArray *row=markdown_table_cells(lines[last]);
        if(row->len!=columns){g_ptr_array_free(row,TRUE);break;}
        g_ptr_array_add(rows,row);last++;
    }
    widths=g_new0(guint,columns);
    for(i=0;i<rows->len;i++){
        GPtrArray *row=g_ptr_array_index(rows,i);
        for(j=0;j<columns;j++)widths[j]=MAX(widths[j],(guint)g_utf8_strlen(g_ptr_array_index(row,j),-1));
    }
    out=g_string_new("");append_table_border(out,widths,columns);
    for(i=0;i<rows->len;i++){
        GPtrArray *row=g_ptr_array_index(rows,i);
        g_string_append_c(out,'|');
        for(j=0;j<columns;j++){
            const gchar *cell=g_ptr_array_index(row,j);
            guint length=(guint)g_utf8_strlen(cell,-1),padding=widths[j]-length;
            g_string_append_printf(out," %s",cell);
            while(padding--)g_string_append_c(out,' ');
            g_string_append(out," |");
        }
        g_string_append_c(out,'\n');
        if(i==0)append_table_border(out,widths,columns);
    }
    append_table_border(out,widths,columns);
    insert_tagged(buffer,out->str,-1,text_tag(buffer,"table"));
    g_string_free(out,TRUE);g_free(widths);g_ptr_array_free(rows,TRUE);
    return last-first;
}

static void insert_markdown(GtkTextBuffer *buffer, const gchar *text) {
    gchar **lines=g_strsplit(text,"\n",-1);
    gboolean code_block=FALSE;
    guint i;
    for(i=0;lines[i];i++){
        const gchar *line=lines[i];
        GtkTextIter start,end;
        gint start_offset;
        GtkTextTag *line_tag=NULL;
        if(g_str_has_prefix(line,"```")){code_block=!code_block;continue;}
        if(!code_block&&lines[i+1]&&strchr(line,'|')&&is_markdown_table_separator(lines[i+1])){
            i+=insert_markdown_table(buffer,lines,i)-1;
            continue;
        }
        start_offset=gtk_text_buffer_get_char_count(buffer);
        if(code_block){insert_tagged(buffer,line,-1,NULL);line_tag=text_tag(buffer,"codeblock");}
        else if(is_markdown_rule(line)){insert_tagged(buffer,"────────────────────────────────────",-1,NULL);line_tag=text_tag(buffer,"rule");}
        else if(line[0]=='#'){
            while(*line=='#')line++;
            if(*line==' ')line++;
            insert_inline_markdown(buffer,line);line_tag=text_tag(buffer,"heading");
        }else if(g_str_has_prefix(line,"> ")){insert_inline_markdown(buffer,line+2);line_tag=text_tag(buffer,"quote");}
        else if(g_str_has_prefix(line,"- ")||g_str_has_prefix(line,"* ")){insert_tagged(buffer,"• ",-1,NULL);insert_inline_markdown(buffer,line+2);}
        else insert_inline_markdown(buffer,line);
        insert_tagged(buffer,"\n",-1,NULL);
        if(line_tag){gtk_text_buffer_get_iter_at_offset(buffer,&start,start_offset);gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_apply_tag(buffer,line_tag,&start,&end);}
    }
    g_strfreev(lines);
}

static void refresh_transcript(App *a) {
    GtkTextBuffer *b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->transcript));GtkTextIter start,end;guint i;gchar*rendered;
    gtk_text_buffer_set_text(b,"",-1);ensure_chat_tags(a,b);
    if(a->current)for(i=0;i<a->current->messages->len;i++){
        Message*m=g_ptr_array_index(a->current->messages,i);gboolean user=!strcmp(m->role,"user");GtkTextTag*role=text_tag(b,user?"user":"assistant");gint message_offset=gtk_text_buffer_get_char_count(b);
        insert_tagged(b,user?"Вы:\n":"ИИ:\n",-1,text_tag(b,"bold"));
        if(user)insert_tagged(b,m->text,-1,NULL);else insert_markdown(b,m->text);
        insert_tagged(b,"\n\n",-1,NULL);gtk_text_buffer_get_iter_at_offset(b,&start,message_offset);gtk_text_buffer_get_end_iter(b,&end);gtk_text_buffer_apply_tag(b,role,&start,&end);
    }
    gtk_text_buffer_get_bounds(b,&start,&end);rendered=gtk_text_buffer_get_text(b,&start,&end,FALSE);apply_emoji_font(a,b,rendered);g_free(rendered);
    gtk_text_buffer_get_end_iter(b,&end);gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(a->transcript),&end,0,FALSE,0,0);
}

static guint estimate_tokens(const gchar *text) {
    const gchar *p=text;
    guint tokens=0, run=0;
    while(p&&*p){
        gunichar ch=g_utf8_get_char(p);
        if(g_unichar_isalnum(ch)){
            run++;
            if(run==4){tokens++;run=0;}
        }else{
            if(run){tokens++;run=0;}
            if(!g_unichar_isspace(ch))tokens++;
        }
        p=g_utf8_next_char(p);
    }
    return tokens+(run?1:0);
}

static void update_chat_context(App *a) {
    guint i,tokens=0;
    gchar *text;
    if(!a->current)return;
    tokens=estimate_tokens(a->current->system_prompt);
    for(i=0;i<a->current->messages->len;i++){
        Message *message=g_ptr_array_index(a->current->messages,i);
        tokens+=4+estimate_tokens(message->role)+estimate_tokens(message->text);
    }
    if(a->token_label){
        text=g_strdup_printf("≈ %u ток.",tokens);
        gtk_label_set_text(GTK_LABEL(a->token_label),text);
        gtk_widget_set_tooltip_text(a->token_label,"Приблизительное число токенов текущего диалога");
        g_free(text);
    }
    if(a->permission_label){
        GString *permissions=g_string_new("Разрешения: ");
        if(!a->current->agent_mode)g_string_append(permissions,"инструменты отключены");
        else if(!a->current->permissions)g_string_append(permissions,"будут запрошены");
        else{
            gboolean added=FALSE;
            if(a->current->permissions&PERMISSION_READ){g_string_append(permissions,"чтение");added=TRUE;}
            if(a->current->permissions&PERMISSION_WRITE){g_string_append(permissions,added?", запись":"запись");added=TRUE;}
            if(a->current->permissions&PERMISSION_MCP)g_string_append(permissions,added?", внешние MCP":"внешние MCP");
        }
        gtk_label_set_text(GTK_LABEL(a->permission_label),permissions->str);
        g_string_free(permissions,TRUE);
    }
    if(a->mode_combo)
        gtk_combo_box_set_active(GTK_COMBO_BOX(a->mode_combo),a->current->agent_mode?0:1);
    if(a->agent_context)
        gtk_widget_set_sensitive(a->agent_context,a->current->agent_mode);
}
static void refresh_list(App *a) {
    gtk_list_store_clear(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list)))); guint i;
    for(i=0;i<a->chats->len;i++){ Chat *c=g_ptr_array_index(a->chats,i); GtkTreeIter it;
        gtk_list_store_append(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list))),&it);
        gtk_list_store_set(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list))),&it,0,c->title,1,i,-1); }
}
static void update_active_agent(App*a){Agent*agent=a->current?find_agent(a,a->current->agent_id):NULL;gchar*text=g_strdup_printf("Агент: %s",agent?agent->name:"не выбран");if(a->agent_label)gtk_label_set_text(GTK_LABEL(a->agent_label),text);g_free(text);}
static void select_chat(App *a, Chat *c) { a->current=c; refresh_transcript(a);update_active_agent(a);update_chat_context(a); }
static void on_selection(GtkTreeSelection *sel,gpointer data){ App *a=data; GtkTreeModel *m; GtkTreeIter it; guint n;
    if(gtk_tree_selection_get_selected(sel,&m,&it)){gtk_tree_model_get(m,&it,1,&n,-1); if(n<a->chats->len)select_chat(a,g_ptr_array_index(a->chats,n));}}
static void new_chat(App *a,gboolean temporary){ Chat *c=chat_new(temporary); g_ptr_array_add(a->chats,c); refresh_list(a); select_chat(a,c); if(!temporary)save_history(a); }
static void on_new(GtkButton *b,gpointer data){(void)b;new_chat(data,FALSE);} static void on_temp(GtkButton *b,gpointer data){(void)b;new_chat(data,TRUE);}

static void on_delete_chat(GtkButton *button, gpointer data) {
    App *a = data;
    GtkWidget *dialog;
    guint index;
    (void)button;

    if (!a->current)
        return;
    if (a->busy) {
        dialog = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Нельзя удалить чат во время получения ответа.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    dialog = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Удалить чат «%s»?\nЭто действие нельзя отменить.", a->current->title);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_YES) {
        gtk_widget_destroy(dialog);
        return;
    }
    gtk_widget_destroy(dialog);

    for (index = 0; index < a->chats->len; index++)
        if (g_ptr_array_index(a->chats, index) == a->current)
            break;
    if (index == a->chats->len)
        return;

    a->current = NULL;
    g_ptr_array_remove_index(a->chats, index);
    if (!a->chats->len) {
        new_chat(a, FALSE);
    } else {
        GtkTreePath *path;
        if (index >= a->chats->len)
            index = a->chats->len - 1;
        refresh_list(a);
        select_chat(a, g_ptr_array_index(a->chats, index));
        path = gtk_tree_path_new_from_indices((gint)index, -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->chat_list)), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(a->chat_list), path, NULL, FALSE, 0, 0);
        gtk_tree_path_free(path);
    }
    schedule_autosave(a);
}

static void refresh_attachments(App *a) {
    GtkListStore *store;
    GtkTreeIter iter;
    guint i;
    gchar *count;
    if(!a->attachments_list)return;
    store=GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->attachments_list)));
    gtk_list_store_clear(store);
    for(i=0;i<a->attachments->len;i++){
        const gchar *path=g_ptr_array_index(a->attachments,i);
        gchar *base=g_path_get_basename(path);
        gtk_list_store_append(store,&iter);
        gtk_list_store_set(store,&iter,0,base,1,path,-1);
        g_free(base);
    }
    count=g_strdup_printf("▣ %u влож.",a->attachments->len);
    gtk_label_set_text(GTK_LABEL(a->attachment_count),count);
    g_free(count);
}

static Agent *find_agent(App*a,const gchar*id){guint i;if(!id)return NULL;for(i=0;i<a->agents->len;i++){Agent*x=g_ptr_array_index(a->agents,i);if(!strcmp(x->id,id))return x;}return NULL;}
static json_object *messages_json(App*a,Chat *c){ json_object *arr=json_object_new_array(),*o; guint i;Agent*agent=c->agent_mode?find_agent(a,c->agent_id):NULL;gchar*prompt;
    prompt=agent&&*agent->system_prompt?(c->system_prompt[0]?g_strdup_printf("%s\n\n%s",agent->system_prompt,c->system_prompt):g_strdup(agent->system_prompt)):g_strdup(c->system_prompt);
    if(prompt[0]){o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string("system"));json_object_object_add(o,"content",json_object_new_string(prompt));json_object_array_add(arr,o);}g_free(prompt);
    for(i=0;i<c->messages->len;i++){Message*m=g_ptr_array_index(c->messages,i);o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string(m->role));json_object_object_add(o,"content",json_object_new_string(m->text));json_object_array_add(arr,o);} return arr;
}
static gboolean deliver_stream(gpointer p) {
    StreamEvent *event = p;
    App *a = event->app;

    if (event->delta && *event->delta) {
        gchar *joined = g_strconcat(event->answer->text, event->delta, NULL);
        set_str(&event->answer->text, joined);
        g_free(joined);
        schedule_autosave(a);
    }
    if (event->error) {
        gchar *shown = event->answer->text[0]
            ? g_strdup_printf("%s\n\n[Ошибка: %s]", event->answer->text, event->error)
            : g_strdup_printf("Ошибка: %s", event->error);
        set_str(&event->answer->text, shown);
        g_free(shown);
    }
    if (a->current == event->chat)
        refresh_transcript(a);
    if (a->current == event->chat)
        update_chat_context(a);
    if (event->done) {
        schedule_autosave(a);
        a->busy = FALSE;
        gtk_widget_set_sensitive(a->send, TRUE);
        gtk_widget_set_sensitive(a->cancel, FALSE);
        gtk_label_set_text(GTK_LABEL(a->status), event->error ? "Ошибка" : "Готово");
        log_event(a,"Сетевой запрос",event->error?"ОШИБКА":"ГОТОВО");
    }
    g_free(event->delta);
    g_free(event->error);
    g_free(event);
    return FALSE;
}

static void queue_stream_event(Job *job, const gchar *delta, const gchar *error, gboolean done) {
    StreamEvent *event = g_new0(StreamEvent, 1);
    event->app = job->app;
    event->chat = job->chat;
    event->answer = job->answer;
    event->delta = g_strdup(delta);
    event->error = g_strdup(error);
    event->done = done;
    g_idle_add(deliver_stream, event);
}

typedef struct { gchar *id; GString *name, *arguments; } ToolCall;
typedef struct {
    App *app;
    Chat *chat;
    gchar *tool_name;
    gchar *details;
    guint permission;
    GMutex mutex;
    GCond cond;
    gboolean answered;
    gboolean allowed;
} PermissionRequest;

static const gchar *permission_name(guint permission) {
    if(permission==PERMISSION_WRITE)return "изменение файлов проекта";
    if(permission==PERMISSION_MCP)return "вызов внешнего MCP-инструмента / выход из песочницы";
    return "чтение файлов проекта";
}

static gboolean show_permission_request(gpointer data) {
    PermissionRequest *request=data;
    GtkWidget *dialog,*area,*remember;
    gint response;
    gchar *secondary;
    dialog=gtk_message_dialog_new(GTK_WINDOW(request->app->window),GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,GTK_BUTTONS_NONE,
        "Агент запрашивает разрешение: %s",permission_name(request->permission));
    secondary=g_strdup_printf("Инструмент: %s\nАргументы: %s",
        request->tool_name,request->details&&*request->details?request->details:"{}");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",secondary);
    g_free(secondary);
    gtk_dialog_add_button(GTK_DIALOG(dialog),"Запретить",GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dialog),"Разрешить один раз",GTK_RESPONSE_ACCEPT);
    gtk_dialog_add_button(GTK_DIALOG(dialog),"Разрешить для этого чата",GTK_RESPONSE_YES);
    area=GTK_DIALOG(dialog)->vbox;
    remember=gtk_label_new("Постоянное разрешение сохраняется только в текущем чате.");
    gtk_misc_set_alignment(GTK_MISC(remember),0,0.5);
    gtk_box_pack_start(GTK_BOX(area),remember,FALSE,FALSE,6);
    gtk_widget_show_all(dialog);
    response=gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_mutex_lock(&request->mutex);
    request->allowed=response==GTK_RESPONSE_ACCEPT||response==GTK_RESPONSE_YES;
    if(response==GTK_RESPONSE_YES){
        request->chat->permissions|=request->permission;
        schedule_autosave(request->app);
        if(request->app->current==request->chat)update_chat_context(request->app);
    }
    request->answered=TRUE;
    g_cond_signal(&request->cond);
    g_mutex_unlock(&request->mutex);
    return FALSE;
}

static gboolean request_tool_permission(Job *job,const gchar *tool_name,json_object *args,guint permission) {
    PermissionRequest *request;
    gboolean allowed;
    if(job->chat->permissions&permission)return TRUE;
    request=g_new0(PermissionRequest,1);
    request->app=job->app;request->chat=job->chat;request->permission=permission;
    request->tool_name=g_strdup(tool_name);
    {
        const gchar *details=json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN);
        request->details=g_utf8_strlen(details,-1)>500?g_strconcat(g_utf8_substring(details,0,500),"…",NULL):g_strdup(details);
    }
    g_mutex_init(&request->mutex);g_cond_init(&request->cond);
    g_mutex_lock(&request->mutex);
    g_idle_add(show_permission_request,request);
    while(!request->answered)g_cond_wait(&request->cond,&request->mutex);
    allowed=request->allowed;
    g_mutex_unlock(&request->mutex);
    g_cond_clear(&request->cond);g_mutex_clear(&request->mutex);
    g_free(request->tool_name);g_free(request->details);g_free(request);
    return allowed;
}

static guint local_tool_permission(const gchar *name) {
    if(!strcmp(name,"write_file")||!strcmp(name,"replace_in_file"))return PERMISSION_WRITE;
    return PERMISSION_READ;
}

typedef struct {
    Job *job;
    GString *pending, *content;
    GPtrArray *calls;
    Buffer raw;
    gboolean openai;
} StreamRound;

static void tool_call_free(gpointer data){
    ToolCall *call=data;if(!call)return;g_free(call->id);
    if(call->name)g_string_free(call->name,TRUE);
    if(call->arguments)g_string_free(call->arguments,TRUE);
    g_free(call);
}
static ToolCall *stream_tool_call(StreamRound *round,guint index){
    while(round->calls->len<=index){
        ToolCall *call=g_new0(ToolCall,1);call->name=g_string_new("");call->arguments=g_string_new("");
        g_ptr_array_add(round->calls,call);
    }
    return g_ptr_array_index(round->calls,index);
}
static void stream_text(StreamRound *round,const gchar *text){
    if(text&&*text){g_string_append(round->content,text);queue_stream_event(round->job,text,NULL,FALSE);}
}
static void parse_stream_message(StreamRound *round,const gchar *line){
    const gchar *json=line;json_object *root,*choices,*choice,*delta,*message,*content,*calls;
    while(g_ascii_isspace(*json))json++;
    if(round->openai){
        if(!g_str_has_prefix(json,"data:"))return;
        json+=5;while(g_ascii_isspace(*json))json++;
        if(!strcmp(json,"[DONE]"))return;
    }
    if(!*json)return;
    root=json_tokener_parse(json);if(!root)return;
    message=NULL;
    if(round->openai){
        if(json_object_object_get_ex(root,"choices",&choices)&&json_object_array_length(choices)){
            choice=json_object_array_get_idx(choices,0);
            if(json_object_object_get_ex(choice,"delta",&delta))message=delta;
        }
    }else if(json_object_object_get_ex(root,"message",&message)){}
    if(message&&json_object_object_get_ex(message,"content",&content)&&json_object_is_type(content,json_type_string))
        stream_text(round,json_object_get_string(content));
    if(message&&json_object_object_get_ex(message,"tool_calls",&calls)&&json_object_is_type(calls,json_type_array)){
        guint i;
        for(i=0;i<json_object_array_length(calls);i++){
            json_object *part=json_object_array_get_idx(calls,i),*value,*function,*args;
            guint index=i;ToolCall *call;
            if(round->openai&&json_object_object_get_ex(part,"index",&value))index=(guint)json_object_get_int(value);
            call=stream_tool_call(round,index);
            if(json_object_object_get_ex(part,"id",&value)&&json_object_is_type(value,json_type_string))set_str(&call->id,json_object_get_string(value));
            if(json_object_object_get_ex(part,"function",&function)){
                if(json_object_object_get_ex(function,"name",&value)&&json_object_is_type(value,json_type_string)){
                    if(round->openai)g_string_append(call->name,json_object_get_string(value));
                    else g_string_assign(call->name,json_object_get_string(value));
                }
                if(json_object_object_get_ex(function,"arguments",&args)){
                    if(json_object_is_type(args,json_type_string))g_string_append(call->arguments,json_object_get_string(args));
                    else g_string_assign(call->arguments,json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN));
                }
            }
        }
    }
    json_object_put(root);
}
static void process_stream_lines(StreamRound *round){
    gchar *newline;
    while((newline=strchr(round->pending->str,'\n'))!=NULL){
        gsize length=(gsize)(newline-round->pending->str);gchar *line=g_strndup(round->pending->str,length);
        if(length&&line[length-1]=='\r')line[length-1]='\0';
        parse_stream_message(round,line);g_free(line);g_string_erase(round->pending,0,length+1);
    }
}
static size_t stream_write_cb(void *p,size_t s,size_t n,void *u){
    StreamRound *round=u;size_t size=s*n;char *next=realloc(round->raw.data,round->raw.len+size+1);
    if(!next)return 0;
    round->raw.data=next;memcpy(next+round->raw.len,p,size);round->raw.len+=size;next[round->raw.len]='\0';
    g_string_append_len(round->pending,p,size);process_stream_lines(round);return size;
}
static int progress_cb(void *data, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
    App *a = data;
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return g_atomic_int_get(&a->cancel_requested) != 0;
}
static gchar *extract_answer(const char *body,gboolean openai){ json_object*r=json_tokener_parse(body),*x,*y,*z; gchar*out=NULL;
    if(!r)
        return NULL;
    if(json_object_object_get_ex(r,"error",&x)){
        if(json_object_is_type(x,json_type_string))out=g_strdup(json_object_get_string(x));
        else if(json_object_object_get_ex(x,"message",&y))out=g_strdup(json_object_get_string(y));
    }
    else if(openai&&json_object_object_get_ex(r,"choices",&x)&&json_object_array_length(x)>0){y=json_object_array_get_idx(x,0);if(json_object_object_get_ex(y,"message",&z)&&json_object_object_get_ex(z,"content",&x))out=g_strdup(json_object_get_string(x));}
    else if(!openai&&json_object_object_get_ex(r,"message",&x)&&json_object_object_get_ex(x,"content",&y))out=g_strdup(json_object_get_string(y));
    json_object_put(r);
    return out;
}
static json_object *assembled_tool_message(StreamRound *round){
    json_object *message=json_object_new_object(),*calls=json_object_new_array();guint i;
    json_object_object_add(message,"role",json_object_new_string("assistant"));
    if(round->content->len)json_object_object_add(message,"content",json_object_new_string(round->content->str));
    else if(round->openai)json_object_object_add(message,"content",NULL);
    else json_object_object_add(message,"content",json_object_new_string(""));
    for(i=0;i<round->calls->len;i++){
        ToolCall *call=g_ptr_array_index(round->calls,i);json_object *item=json_object_new_object(),*function=json_object_new_object(),*args;
        if(!call->name->len)continue;
        if(round->openai){
            json_object_object_add(item,"id",json_object_new_string(call->id?call->id:"call_unknown"));
            json_object_object_add(item,"type",json_object_new_string("function"));
            json_object_object_add(function,"arguments",json_object_new_string(call->arguments->str));
        }else{
            args=json_tokener_parse(call->arguments->str);
            json_object_object_add(function,"arguments",args?args:json_object_new_object());
        }
        json_object_object_add(function,"name",json_object_new_string(call->name->str));
        json_object_object_add(item,"function",function);json_object_array_add(calls,item);
    }
    json_object_object_add(message,"tool_calls",calls);return message;
}
static gpointer worker(gpointer p){
    Job*j=p;json_object*request=json_tokener_parse(j->request),*messages;
    gchar*error=NULL;gboolean final_received=FALSE;guint round;
    if(!request||!json_object_object_get_ex(request,"messages",&messages)){error=g_strdup("Не удалось создать запрос инструментов");goto done;}
    for(round=0;round<12&&!error&&!final_received;round++){
        CURL*c=curl_easy_init();struct curl_slist*h=NULL;CURLcode rc=CURLE_FAILED_INIT;long status=0;
        StreamRound stream={j,g_string_new(""),g_string_new(""),g_ptr_array_new_with_free_func(tool_call_free),{calloc(1,1),0},j->openai};
        gchar*payload=g_strdup(json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN));
        if(c){h=curl_slist_append(h,"Content-Type: application/json");if(j->openai&&j->key[0]){gchar*auth=g_strdup_printf("Authorization: Bearer %s",j->key);h=curl_slist_append(h,auth);g_free(auth);}
            curl_easy_setopt(c,CURLOPT_URL,j->url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);curl_easy_setopt(c,CURLOPT_POSTFIELDS,payload);
            curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,stream_write_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&stream);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,300L);
            curl_easy_setopt(c,CURLOPT_USERAGENT,"gtk2aichat/0.4");curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress_cb);curl_easy_setopt(c,CURLOPT_XFERINFODATA,j->app);
            rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);}
        if(stream.pending->len){g_string_append_c(stream.pending,'\n');process_stream_lines(&stream);}
        if(rc==CURLE_ABORTED_BY_CALLBACK)error=g_strdup("Запрос отменён");
        else if(rc!=CURLE_OK)error=g_strdup_printf("Сетевая ошибка: %s",curl_easy_strerror(rc));
        else if(status<200||status>=300){gchar*api=extract_answer(stream.raw.data?stream.raw.data:"",j->openai);error=api?api:g_strdup_printf("HTTP %ld: %.500s",status,stream.raw.data?stream.raw.data:"");}
        g_free(payload);curl_slist_free_all(h);if(c)curl_easy_cleanup(c);
        if(!error&&stream.calls->len){
            guint i;json_object *assistant=assembled_tool_message(&stream);
            json_object_array_add(messages,assistant);
            for(i=0;i<stream.calls->len;i++){
                ToolCall*call=g_ptr_array_index(stream.calls,i);json_object*args=NULL,*tool_message;const gchar*name,*id;gchar*result;
                if(!call->name->len)continue;
                name=call->name->str;id=call->id;
                args=json_tokener_parse(call->arguments->str);
                if(!args)args=json_object_new_object();
                if(mcp_manager_has_tool(j->app->mcp,name)){
                    if(request_tool_permission(j,name,args,PERMISSION_MCP))result=mcp_manager_call(j->app->mcp,name,args);
                    else result=g_strdup("ERROR: user denied permission for external MCP tool");
                }else{
                    guint permission=local_tool_permission(name);
                    if(request_tool_permission(j,name,args,permission))
                        result=agent_tool_execute(name,args,j->app->project_root,TRUE,permission==PERMISSION_WRITE);
                    else result=g_strdup("ERROR: user denied tool permission");
                }
                if((!strcmp(name,"write_file")||!strcmp(name,"replace_in_file"))&&!g_str_has_prefix(result,"ERROR:")){
                    json_object *path_value;
                    if(json_object_object_get_ex(args,"path",&path_value)&&json_object_is_type(path_value,json_type_string)){
                        const gchar *changed_path=json_object_get_string(path_value);
                        queue_diff_event(j->app,changed_path,agent_tool_file_diff(j->app->project_root,changed_path));
                    }
                }
                tool_message=json_object_new_object();json_object_object_add(tool_message,"role",json_object_new_string("tool"));json_object_object_add(tool_message,"content",json_object_new_string(result));
                if(id)json_object_object_add(tool_message,"tool_call_id",json_object_new_string(id));
                if(!j->openai)json_object_object_add(tool_message,"tool_name",json_object_new_string(name));
                json_object_array_add(messages,tool_message);queue_log_event(j->app,name,g_str_has_prefix(result,"ERROR:")?"ОШИБКА":"ГОТОВО");
                g_free(result);json_object_put(args);
            }
            if(stream.content->len)queue_stream_event(j,"\n\n",NULL,FALSE);
        }else if(!error&&stream.content->len)final_received=TRUE;
        else if(!error)error=g_strdup("Модель не вернула текст или вызов инструмента");
        free(stream.raw.data);g_string_free(stream.pending,TRUE);g_string_free(stream.content,TRUE);g_ptr_array_free(stream.calls,TRUE);
    }
    if(!error&&!final_received)error=g_strdup("Превышен лимит из 12 последовательных вызовов инструментов");
done:
    queue_stream_event(j,NULL,error,TRUE);g_free(error);if(request)json_object_put(request);
    g_free(j->request);g_free(j->url);g_free(j->key);g_free(j);return NULL;
}
static void on_send(GtkButton*b,gpointer data){
    App*a=data;GtkTextBuffer*tb;GtkTextIter s,e;gchar*text;json_object*root;Job*j;Message*answer;Agent*agent;const gchar*provider,*model;
    (void)b;
    if(a->busy||!a->current)return;
    tb=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->input));
    gtk_text_buffer_get_bounds(tb,&s,&e);
    text=gtk_text_buffer_get_text(tb,&s,&e,FALSE);
    g_strstrip(text);
    if(!*text){g_free(text);return;}
    if(a->attachments->len){
        GString *expanded=g_string_new(text);
        guint attachment_index;
        for(attachment_index=0;attachment_index<a->attachments->len;attachment_index++){
            const gchar *path=g_ptr_array_index(a->attachments,attachment_index);
            gchar *contents=NULL;
            gsize length=0;
            g_string_append_printf(expanded,"\n\n--- Вложение: %s ---\n",path);
            if(g_file_get_contents(path,&contents,&length,NULL)){
                if(length>65536)length=65536;
                g_string_append_len(expanded,contents,length);
                if(length==65536)g_string_append(expanded,"\n[файл обрезан до 64 КиБ]");
            }else g_string_append(expanded,"[не удалось прочитать файл]");
            g_free(contents);
        }
        g_free(text);
        text=g_string_free(expanded,FALSE);
        g_ptr_array_set_size(a->attachments,0);
        refresh_attachments(a);
    }
    add_message(a->current,"user",text);
    if(a->current->messages->len==1&&!a->current->temporary){
        gchar*t=g_utf8_substring(text,0,MIN(36,g_utf8_strlen(text,-1)));
        set_str(&a->current->title,t);g_free(t);refresh_list(a);
    }
    agent=a->current->agent_mode?find_agent(a,a->current->agent_id):NULL;provider=agent&&*agent->provider?agent->provider:a->provider;
    model=agent&&*agent->model?agent->model:(!strcmp(provider,"openai")?a->openai_model:a->ollama_model);
    root=json_object_new_object();
    json_object_object_add(root,"model",json_object_new_string(model));
    json_object_object_add(root,"messages",messages_json(a,a->current));
    json_object_object_add(root,"stream",json_object_new_boolean(TRUE));
    if(a->current->agent_mode){
        json_object*tools=agent_tools_schema(),*mcp_tools=mcp_manager_tools_schema(a->mcp);guint tool_index;
        for(tool_index=0;tool_index<json_object_array_length(mcp_tools);tool_index++)json_object_array_add(tools,json_object_get(json_object_array_get_idx(mcp_tools,tool_index)));
        json_object_put(mcp_tools);json_object_object_add(root,"tools",tools);
    }
    j=g_new0(Job,1);j->app=a;j->chat=a->current;j->openai=!strcmp(provider,"openai");
    j->url=g_strdup(j->openai?a->openai_url:a->ollama_url);j->key=g_strdup(a->openai_key);
    j->request=g_strdup(json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    add_message(a->current,"assistant","");
    answer=g_ptr_array_index(a->current->messages,a->current->messages->len-1);
    j->answer=answer;
    gtk_text_buffer_set_text(tb,"",-1);refresh_transcript(a);
    update_chat_context(a);
    if(!a->current->temporary)save_history(a);
    a->busy=TRUE;g_atomic_int_set(&a->cancel_requested,0);gtk_widget_set_sensitive(a->send,FALSE);gtk_widget_set_sensitive(a->cancel,TRUE);
    gtk_label_set_text(GTK_LABEL(a->status),"Получение ответа…");
    log_event(a,!strcmp(provider,"openai")?"OpenAI · chat":"Ollama · chat","ВЫПОЛНЯЕТСЯ");
    g_thread_unref(g_thread_new("ai-request",worker,j));g_free(text);
}

static gboolean on_input_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    gboolean enter = event->keyval == GDK_Return || event->keyval == GDK_KP_Enter;
#ifdef GDK_ISO_Enter
    enter = enter || event->keyval == GDK_ISO_Enter;
#endif
    (void)widget;
    if (enter && !(event->state & GDK_SHIFT_MASK)) {
        on_send(NULL, data);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *entry_row(GtkTable*t,int row,const char*label,const char*value){GtkWidget*l=gtk_label_new(label),*e=gtk_entry_new();gtk_misc_set_alignment(GTK_MISC(l),0,0.5);gtk_entry_set_text(GTK_ENTRY(e),value?value:"");gtk_table_attach(t,l,0,1,row,row+1,GTK_FILL,GTK_FILL,4,3);gtk_table_attach(t,e,1,2,row,row+1,GTK_EXPAND|GTK_FILL,GTK_FILL,4,3);return e;}
static void on_show_api_key(GtkToggleButton *toggle, gpointer data) {
    gtk_entry_set_visibility(GTK_ENTRY(data), gtk_toggle_button_get_active(toggle));
}

static GtkWidget *color_row(GtkTable *table, gint row, const gchar *label, const gchar *value) {
    GdkColor color;
    GtkWidget *title=gtk_label_new(label),*button;
    if(!gdk_color_parse(value,&color))gdk_color_parse("#000000",&color);
    button=gtk_color_button_new_with_color(&color);
    gtk_misc_set_alignment(GTK_MISC(title),0,0.5);
    gtk_table_attach(table,title,0,1,row,row+1,GTK_FILL,GTK_FILL,4,3);
    gtk_table_attach(table,button,1,2,row,row+1,GTK_FILL,GTK_FILL,4,3);
    return button;
}

static gchar *color_button_value(GtkWidget *button) {
    GdkColor color;
    gtk_color_button_get_color(GTK_COLOR_BUTTON(button),&color);
    return g_strdup_printf("#%02x%02x%02x",color.red/257,color.green/257,color.blue/257);
}

static void on_settings(GtkButton*b,gpointer data){
    App*a=data;GtkWidget*d,*t,*combo,*sys,*ou,*ok,*om,*lu,*lm,*ef,*show_key,*uc,*ac;
    (void)b;
    d=gtk_dialog_new_with_buttons("Настройки",GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_SAVE,GTK_RESPONSE_OK,NULL);
    t=gtk_table_new(11,2,FALSE);combo=gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo),"ollama");gtk_combo_box_append_text(GTK_COMBO_BOX(combo),"openai");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo),!strcmp(a->provider,"openai"));
    gtk_table_attach(GTK_TABLE(t),gtk_label_new("Провайдер"),0,1,0,1,GTK_FILL,GTK_FILL,4,3);gtk_table_attach(GTK_TABLE(t),combo,1,2,0,1,GTK_FILL,GTK_FILL,4,3);
    ou=entry_row(GTK_TABLE(t),1,"OpenAI URL",a->openai_url);ok=entry_row(GTK_TABLE(t),2,"OpenAI API key",a->openai_key);gtk_entry_set_visibility(GTK_ENTRY(ok),FALSE);
    show_key=gtk_check_button_new_with_label("Показать OpenAI API key");gtk_table_attach(GTK_TABLE(t),show_key,1,2,3,4,GTK_FILL,GTK_FILL,4,0);g_signal_connect(show_key,"toggled",G_CALLBACK(on_show_api_key),ok);
    om=entry_row(GTK_TABLE(t),4,"OpenAI модель",a->openai_model);lu=entry_row(GTK_TABLE(t),5,"Ollama URL",a->ollama_url);lm=entry_row(GTK_TABLE(t),6,"Ollama модель",a->ollama_model);
    ef=entry_row(GTK_TABLE(t),7,"Шрифт эмодзи",a->emoji_font);uc=color_row(GTK_TABLE(t),8,"Цвет человека",a->user_color);ac=color_row(GTK_TABLE(t),9,"Цвет ИИ",a->assistant_color);
    sys=entry_row(GTK_TABLE(t),10,"Системный промпт",a->current?a->current->system_prompt:"");gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox),t,TRUE,TRUE,6);gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_OK){
        gchar*user_color=color_button_value(uc),*assistant_color=color_button_value(ac);
        set_str(&a->provider,gtk_combo_box_get_active(GTK_COMBO_BOX(combo))?"openai":"ollama");set_str(&a->openai_url,gtk_entry_get_text(GTK_ENTRY(ou)));set_str(&a->openai_key,gtk_entry_get_text(GTK_ENTRY(ok)));set_str(&a->openai_model,gtk_entry_get_text(GTK_ENTRY(om)));set_str(&a->ollama_url,gtk_entry_get_text(GTK_ENTRY(lu)));set_str(&a->ollama_model,gtk_entry_get_text(GTK_ENTRY(lm)));set_str(&a->emoji_font,gtk_entry_get_text(GTK_ENTRY(ef)));set_str(&a->user_color,user_color);set_str(&a->assistant_color,assistant_color);g_free(user_color);g_free(assistant_color);a->emoji_font_checked=FALSE;
        if(a->current)
            set_str(&a->current->system_prompt,gtk_entry_get_text(GTK_ENTRY(sys)));
        refresh_transcript(a);
        schedule_autosave(a);
    }
    gtk_widget_destroy(d);
}

static void on_emoji_pick(GtkMenuItem *item, gpointer data) {
    App *a = data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->input));
    gtk_text_buffer_insert_at_cursor(buffer, gtk_menu_item_get_label(item), -1);
    gtk_widget_grab_focus(a->input);
}

static void on_emoji(GtkButton *button, gpointer data) {
    static const gchar *emoji[] = {
        "😀", "😂", "😊", "😍", "🤔", "👍", "👎", "❤️",
        "🔥", "🎉", "✅", "❌", "⚠️", "💡", "🚀", "🙏", NULL
    };
    GtkWidget *menu = gtk_menu_new();
    guint i;
    (void)button;
    for (i = 0; emoji[i]; i++) {
        GtkWidget *item = gtk_menu_item_new_with_label(emoji[i]);
        g_signal_connect(item, "activate", G_CALLBACK(on_emoji_pick), data);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time());
}

static gboolean on_delete_window(GtkWidget *widget, GdkEvent *event, gpointer data) {
    App *a = data;
    (void)widget;
    (void)event;
    if (a->busy) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(a->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Дождитесь окончания ответа перед закрытием приложения.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer data) {
    App *a = data;
    (void)widget;
    a->window_width = event->width;
    a->window_height = event->height;
    schedule_autosave(a);
    return FALSE;
}

static void on_paned_position(GObject *object, GParamSpec *spec, gpointer data) {
    App *a = data;
    (void)object;
    (void)spec;
    a->paned_position = gtk_paned_get_position(GTK_PANED(a->paned));
    schedule_autosave(a);
}

static GtkWidget *scroll(GtkWidget*w){GtkWidget*s=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);gtk_container_add(GTK_CONTAINER(s),w);return s;}
static GtkWidget *left_label(const gchar *text){GtkWidget*l=gtk_label_new(text);gtk_misc_set_alignment(GTK_MISC(l),0,0.5);return l;}
static GtkWidget *section_title(const gchar *text){GtkWidget*l=left_label(text);PangoAttrList*attrs=pango_attr_list_new();pango_attr_list_insert(attrs,pango_attr_weight_new(PANGO_WEIGHT_BOLD));gtk_label_set_attributes(GTK_LABEL(l),attrs);pango_attr_list_unref(attrs);return l;}
static GtkWidget *menu_item(GtkWidget *menu,const gchar *label,GCallback callback,App *a){
    GtkWidget *item=gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),item);
    if(callback)g_signal_connect(item,"activate",callback,a);
    return item;
}
static void on_quit(GtkMenuItem *item,gpointer data){App*a=data;(void)item;if(!on_delete_window(a->window,NULL,a))gtk_widget_destroy(a->window);}
static void on_cancel(GtkButton *button,gpointer data){App*a=data;(void)button;if(a->busy){g_atomic_int_set(&a->cancel_requested,1);gtk_label_set_text(GTK_LABEL(a->status),"Отмена запроса…");log_event(a,"Сетевой запрос","ОТМЕНА");}}
static void on_view(GtkButton *button,gpointer data){App*a=data;gint page=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"page"));gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook),page);}
static void on_menu_view(GtkMenuItem *item,gpointer data){App*a=data;gint page=GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item),"page"));gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook),page);}
static void on_left_panel_toggle(GtkToggleButton*toggle,gpointer data){App*a=data;a->left_panel_visible=gtk_toggle_button_get_active(toggle);if(a->left_panel_visible)gtk_widget_show(a->left_panel);else gtk_widget_hide(a->left_panel);schedule_autosave(a);}
static void on_right_panel_toggle(GtkToggleButton*toggle,gpointer data){App*a=data;a->right_panel_visible=gtk_toggle_button_get_active(toggle);if(a->right_panel_visible)gtk_widget_show(a->right_panel);else gtk_widget_hide(a->right_panel);schedule_autosave(a);}
static void on_provider_changed(GtkComboBox *combo,gpointer data){
    App*a=data;gint selected=gtk_combo_box_get_active(combo);
    set_str(&a->provider,selected==1?"openai":"ollama");
    gtk_entry_set_text(GTK_ENTRY(a->model_entry),selected==1?a->openai_model:a->ollama_model);
    schedule_autosave(a);
}
static void on_model_changed(GtkEditable *editable,gpointer data){
    App*a=data;const gchar*model=gtk_entry_get_text(GTK_ENTRY(editable));
    if(!strcmp(a->provider,"openai"))set_str(&a->openai_model,model);else set_str(&a->ollama_model,model);
    schedule_autosave(a);
}
static void on_mode_changed(GtkComboBox *combo,gpointer data){
    App*a=data;gint selected;
    if(!a->current)return;
    selected=gtk_combo_box_get_active(combo);
    if(selected<0)return;
    a->current->agent_mode=selected==0;
    update_chat_context(a);
    schedule_autosave(a);
}
static void on_reset_permissions(GtkButton *button,gpointer data){
    App*a=data;(void)button;
    if(!a->current||a->busy)return;
    a->current->permissions=0;
    update_chat_context(a);
    schedule_autosave(a);
}
static void on_attach(GtkButton *button,gpointer data){
    App*a=data;GtkWidget*d;GSList*files,*p;(void)button;
    d=gtk_file_chooser_dialog_new("Добавить вложения",GTK_WINDOW(a->window),GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_ADD,GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d),TRUE);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){
        files=gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));
        for(p=files;p;p=p->next)g_ptr_array_add(a->attachments,p->data);
        g_slist_free(files);refresh_attachments(a);log_event(a,"Добавление вложений","ГОТОВО");
    }
    gtk_widget_destroy(d);
}
static void update_mcp_label(App *a){
    gchar *status;
    if(!a->mcp_label)return;
    status=g_strdup_printf("MCP: %u серверов · %u инструментов",
        mcp_manager_server_count(a->mcp),mcp_manager_tool_count(a->mcp));
    gtk_label_set_text(GTK_LABEL(a->mcp_label),status);g_free(status);
}
static void reload_mcp(App *a){
    gchar *error=NULL;
    if(!mcp_manager_reload(a->mcp,&error))gtk_label_set_text(GTK_LABEL(a->status),error);
    else{update_mcp_label(a);log_event(a,"MCP discovery","ГОТОВО");}
    g_free(error);
}
static void on_mcp_dialog(GtkButton *button,gpointer data){
    App*a=data;GtkWidget*d,*label;gchar*text,*skills;json_object*empty=json_object_new_object();(void)button;
    skills=agent_tool_execute("list_skills",empty,a->project_root,TRUE,FALSE);json_object_put(empty);
    text=g_strdup_printf("Конфигурация:\n%s\n\n"
        "Формат:\n{\"servers\":[{\"name\":\"filesystem\",\"command\":\"имя-команды\",\"args\":[\"аргумент\"]}]}\n\n"
        "Подключено серверов: %u\nДоступно MCP-инструментов: %u\n\nSkills:\n%s",
        mcp_manager_config_path(a->mcp),mcp_manager_server_count(a->mcp),mcp_manager_tool_count(a->mcp),skills);
    d=gtk_dialog_new_with_buttons("MCP и skills",GTK_WINDOW(a->window),GTK_DIALOG_MODAL,
        GTK_STOCK_REFRESH,GTK_RESPONSE_APPLY,GTK_STOCK_CLOSE,GTK_RESPONSE_CLOSE,NULL);
    label=left_label(text);gtk_label_set_selectable(GTK_LABEL(label),TRUE);gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
    gtk_misc_set_padding(GTK_MISC(label),12,12);gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox),label,TRUE,TRUE,0);gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_APPLY){
        if(a->busy)gtk_label_set_text(GTK_LABEL(a->status),"Дождитесь завершения ответа перед перезагрузкой MCP");
        else reload_mcp(a);
    }
    gtk_widget_destroy(d);g_free(text);g_free(skills);
}
static void on_choose_project(GtkButton *button,gpointer data){
    App*a=data;GtkWidget*d;(void)button;
    if(a->busy)return;
    d=gtk_file_chooser_dialog_new("Выбрать папку проекта",GTK_WINDOW(a->window),GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_OPEN,GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(d),a->project_root);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_ACCEPT){
        gchar*selected=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        gchar*canonical=g_canonicalize_filename(selected,NULL);
        set_str(&a->project_root,canonical);gtk_label_set_text(GTK_LABEL(a->project_label),a->project_root);
        mcp_manager_free(a->mcp);a->mcp=mcp_manager_new(a->project_root);reload_mcp(a);
        g_free(canonical);g_free(selected);schedule_autosave(a);log_event(a,"Выбор проекта","ГОТОВО");
    }
    gtk_widget_destroy(d);
}
static GtkWidget *build_menubar(App *a){
    GtkWidget *bar=gtk_menu_bar_new(),*root,*menu,*item;guint i;
    const gchar *views[]={"Разговор","Агенты","Журнал инструментов","Изменения"};
    root=gtk_menu_item_new_with_mnemonic("_Файл");menu=gtk_menu_new();gtk_menu_item_set_submenu(GTK_MENU_ITEM(root),menu);gtk_menu_shell_append(GTK_MENU_SHELL(bar),root);
    menu_item(menu,"_Новый чат",G_CALLBACK(on_new),a);menu_item(menu,"_Временный чат",G_CALLBACK(on_temp),a);menu_item(menu,"_Удалить чат",G_CALLBACK(on_delete_chat),a);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),gtk_separator_menu_item_new());menu_item(menu,"В_ыход",G_CALLBACK(on_quit),a);
    root=gtk_menu_item_new_with_mnemonic("_Правка");menu=gtk_menu_new();gtk_menu_item_set_submenu(GTK_MENU_ITEM(root),menu);gtk_menu_shell_append(GTK_MENU_SHELL(bar),root);
    menu_item(menu,"Настройки…",G_CALLBACK(on_settings),a);
    root=gtk_menu_item_new_with_mnemonic("_Вид");menu=gtk_menu_new();gtk_menu_item_set_submenu(GTK_MENU_ITEM(root),menu);gtk_menu_shell_append(GTK_MENU_SHELL(bar),root);
    for(i=0;i<4;i++){item=menu_item(menu,views[i],G_CALLBACK(on_menu_view),a);g_object_set_data(G_OBJECT(item),"page",GINT_TO_POINTER(i));}
    root=gtk_menu_item_new_with_mnemonic("_Справка");menu=gtk_menu_new();gtk_menu_item_set_submenu(GTK_MENU_ITEM(root),menu);gtk_menu_shell_append(GTK_MENU_SHELL(bar),root);
    menu_item(menu,"Agent Desk · GTK2 AI Desktop",NULL,a);
    return bar;
}
static void refresh_agents(App*a){
    GtkTreeIter it;guint i;gtk_list_store_clear(a->agent_store);
    for(i=0;i<a->agents->len;i++){Agent*x=g_ptr_array_index(a->agents,i);gtk_list_store_append(a->agent_store,&it);
        gtk_list_store_set(a->agent_store,&it,0,x->name,1,x->description,2,x->provider,3,x->model,4,x->id,-1);}
    if(a->agent_empty){if(a->agents->len)gtk_widget_hide(a->agent_empty);else gtk_widget_show(a->agent_empty);}
}
static Agent *selected_agent(GtkTreeView*view,App*a){
    GtkTreeModel*m;GtkTreeIter it;gchar*id=NULL;Agent*agent=NULL;
    if(gtk_tree_selection_get_selected(gtk_tree_view_get_selection(view),&m,&it)){gtk_tree_model_get(m,&it,4,&id,-1);agent=find_agent(a,id);g_free(id);}return agent;
}
static gboolean agent_dialog(App*a,Agent*agent){
    GtkWidget*d,*table,*name,*description,*prompt,*provider,*model;gboolean saved=FALSE;
    d=gtk_dialog_new_with_buttons(agent?"Редактировать агента":"Новый агент",GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_SAVE,GTK_RESPONSE_OK,NULL);
    table=gtk_table_new(5,2,FALSE);name=entry_row(GTK_TABLE(table),0,"Имя",agent?agent->name:"");
    description=entry_row(GTK_TABLE(table),1,"Описание",agent?agent->description:"");
    prompt=entry_row(GTK_TABLE(table),2,"Системная инструкция",agent?agent->system_prompt:"");
    provider=gtk_combo_box_new_text();gtk_combo_box_append_text(GTK_COMBO_BOX(provider),"ollama");gtk_combo_box_append_text(GTK_COMBO_BOX(provider),"openai");gtk_combo_box_set_active(GTK_COMBO_BOX(provider),agent&&!strcmp(agent->provider,"openai"));
    gtk_table_attach(GTK_TABLE(table),gtk_label_new("Провайдер"),0,1,3,4,GTK_FILL,GTK_FILL,4,3);gtk_table_attach(GTK_TABLE(table),provider,1,2,3,4,GTK_FILL,GTK_FILL,4,3);
    model=entry_row(GTK_TABLE(table),4,"Модель",agent?agent->model:(!strcmp(a->provider,"openai")?a->openai_model:a->ollama_model));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox),table,TRUE,TRUE,8);gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_OK&&*gtk_entry_get_text(GTK_ENTRY(name))){
        if(!agent){agent=g_new0(Agent,1);agent->id=g_strdup_printf("agent-%"G_GINT64_FORMAT,g_get_real_time());g_ptr_array_add(a->agents,agent);}
        set_str(&agent->name,gtk_entry_get_text(GTK_ENTRY(name)));set_str(&agent->description,gtk_entry_get_text(GTK_ENTRY(description)));
        set_str(&agent->system_prompt,gtk_entry_get_text(GTK_ENTRY(prompt)));set_str(&agent->provider,gtk_combo_box_get_active(GTK_COMBO_BOX(provider))?"openai":"ollama");
        set_str(&agent->model,gtk_entry_get_text(GTK_ENTRY(model)));save_agents(a);refresh_agents(a);saved=TRUE;
    }
    gtk_widget_destroy(d);return saved;
}
typedef struct{App*app;GtkTreeView*view;} AgentAction;
static void on_agent_new(GtkButton*b,gpointer data){AgentAction*x=data;(void)b;agent_dialog(x->app,NULL);}
static void on_agent_edit(GtkButton*b,gpointer data){AgentAction*x=data;Agent*a=selected_agent(x->view,x->app);(void)b;if(a)agent_dialog(x->app,a);}
static void on_agent_use(GtkButton*b,gpointer data){AgentAction*x=data;Agent*a=selected_agent(x->view,x->app);(void)b;
    if(a&&x->app->current){set_str(&x->app->current->agent_id,a->id);update_active_agent(x->app);schedule_autosave(x->app);gtk_notebook_set_current_page(GTK_NOTEBOOK(x->app->notebook),0);}}
static void on_agent_delete(GtkButton*b,gpointer data){AgentAction*x=data;Agent*agent=selected_agent(x->view,x->app);GtkWidget*d;guint i;(void)b;if(!agent)return;
    d=gtk_message_dialog_new(GTK_WINDOW(x->app->window),GTK_DIALOG_MODAL,GTK_MESSAGE_QUESTION,GTK_BUTTONS_YES_NO,"Удалить агента «%s»?",agent->name);
    if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_YES){for(i=0;i<x->app->chats->len;i++){Chat*c=g_ptr_array_index(x->app->chats,i);if(c->agent_id&&!strcmp(c->agent_id,agent->id))set_str(&c->agent_id,"");}
        for(i=0;i<x->app->agents->len;i++){
            if(g_ptr_array_index(x->app->agents,i)==agent){g_ptr_array_remove_index(x->app->agents,i);break;}
        }
        save_agents(x->app);save_history(x->app);refresh_agents(x->app);update_active_agent(x->app);
    }
    gtk_widget_destroy(d);
}
static GtkWidget *build_agents_page(App*a){
    GtkWidget*outer=gtk_vbox_new(FALSE,5),*buttons=gtk_table_new(2,2,TRUE),*view;GtkCellRenderer*r=gtk_cell_renderer_text_new();AgentAction*action=g_new0(AgentAction,1);GtkWidget*b;
    a->agent_store=gtk_list_store_new(5,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING);
    view=gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->agent_store));gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes("Имя",r,"text",0,NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes("Описание",r,"text",1,NULL));gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes("Провайдер",r,"text",2,NULL));gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes("Модель",r,"text",3,NULL));
    action->app=a;action->view=GTK_TREE_VIEW(view);g_object_set_data_full(G_OBJECT(outer),"agent-action",action,g_free);
    gtk_table_set_row_spacings(GTK_TABLE(buttons),4);gtk_table_set_col_spacings(GTK_TABLE(buttons),4);
    b=gtk_button_new_with_label("Создать…");g_signal_connect(b,"clicked",G_CALLBACK(on_agent_new),action);gtk_table_attach_defaults(GTK_TABLE(buttons),b,0,1,0,1);
    b=gtk_button_new_with_label("Редактировать…");g_signal_connect(b,"clicked",G_CALLBACK(on_agent_edit),action);gtk_table_attach_defaults(GTK_TABLE(buttons),b,1,2,0,1);
    b=gtk_button_new_with_label("Удалить");g_signal_connect(b,"clicked",G_CALLBACK(on_agent_delete),action);gtk_table_attach_defaults(GTK_TABLE(buttons),b,0,1,1,2);
    b=gtk_button_new_with_label("Использовать в чате");g_signal_connect(b,"clicked",G_CALLBACK(on_agent_use),action);gtk_table_attach_defaults(GTK_TABLE(buttons),b,1,2,1,2);
    gtk_container_set_border_width(GTK_CONTAINER(outer),10);gtk_box_pack_start(GTK_BOX(outer),section_title("Профили агентов"),FALSE,FALSE,0);a->agent_empty=left_label("Профилей пока нет. Создайте агента и назначьте его текущему чату.");gtk_box_pack_start(GTK_BOX(outer),a->agent_empty,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(outer),scroll(view),TRUE,TRUE,0);gtk_box_pack_end(GTK_BOX(outer),buttons,FALSE,FALSE,0);refresh_agents(a);return outer;
}
static GtkWidget *build_log_page(App *a){
    GtkListStore *store=gtk_list_store_new(4,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING);
    GtkWidget *view=gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));GtkCellRenderer*r=gtk_cell_renderer_text_new();GtkTreeIter it;
    const gchar *titles[]={"Время","Агент / инструмент","Статус","Длительность"};guint i;
    for(i=0;i<4;i++)gtk_tree_view_append_column(GTK_TREE_VIEW(view),gtk_tree_view_column_new_with_attributes(titles[i],r,"text",i,NULL));
    gtk_list_store_append(store,&it);gtk_list_store_set(store,&it,0,"—",1,"Agent Desk",2,"ГОТОВО",3,"запуск",-1);
    a->log_store=store;
    return scroll(view);
}
static void show_diff(App*a,const gchar*diff){
    GtkTextBuffer*b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->changes_view));GtkTextIter start,end;gchar**lines;guint i;gint offset=0;
    gtk_text_buffer_set_text(b,diff?diff:"",-1);
    if(!gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b),"diff-add"))gtk_text_buffer_create_tag(b,"diff-add","foreground","#2e7d32",NULL);
    if(!gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b),"diff-del"))gtk_text_buffer_create_tag(b,"diff-del","foreground","#c62828",NULL);
    if(!gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(b),"diff-head"))gtk_text_buffer_create_tag(b,"diff-head","foreground","#3465a4","weight",PANGO_WEIGHT_BOLD,NULL);
    lines=g_strsplit(diff?diff:"","\n",-1);
    for(i=0;lines[i];i++){gint length=g_utf8_strlen(lines[i],-1)+1;const gchar*tag=NULL;
        if(lines[i][0]=='+'&&lines[i][1]!='+')tag="diff-add";else if(lines[i][0]=='-'&&lines[i][1]!='-')tag="diff-del";else if(g_str_has_prefix(lines[i],"@@")||g_str_has_prefix(lines[i],"diff "))tag="diff-head";
        if(tag){gtk_text_buffer_get_iter_at_offset(b,&start,offset);gtk_text_buffer_get_iter_at_offset(b,&end,offset+length);gtk_text_buffer_apply_tag_by_name(b,tag,&start,&end);}
        offset+=length;
    }g_strfreev(lines);
}
static void on_change_selection(GtkTreeSelection*selection,gpointer data){
    App*a=data;GtkTreeModel*m;GtkTreeIter it;gchar*path=NULL;
    if(gtk_tree_selection_get_selected(selection,&m,&it)){gtk_tree_model_get(m,&it,0,&path,-1);show_diff(a,g_hash_table_lookup(a->changes,path));g_free(path);}
}
static GtkWidget *build_changes_page(App*a){
    GtkWidget*paned=gtk_hpaned_new(),*tree;GtkCellRenderer*r=gtk_cell_renderer_text_new();
    a->changes_store=gtk_list_store_new(1,G_TYPE_STRING);tree=gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->changes_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree),gtk_tree_view_column_new_with_attributes("Изменённые файлы",r,"text",0,NULL));
    a->changes_view=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(a->changes_view),FALSE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->changes_view),GTK_WRAP_NONE);
    {PangoFontDescription*font=pango_font_description_from_string("monospace 9");gtk_widget_modify_font(a->changes_view,font);pango_font_description_free(font);}
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)),"changed",G_CALLBACK(on_change_selection),a);
    gtk_paned_pack1(GTK_PANED(paned),scroll(tree),FALSE,FALSE);gtk_paned_pack2(GTK_PANED(paned),scroll(a->changes_view),TRUE,FALSE);gtk_paned_set_position(GTK_PANED(paned),220);
    return paned;
}
static void build_ui(App*a){
    GtkWidget *v=gtk_vbox_new(FALSE,0),*tools=gtk_hbox_new(FALSE,5),*paned=gtk_hpaned_new(),*sidebar=gtk_vbox_new(FALSE,0);
    GtkWidget *right=gtk_vbox_new(FALSE,0),*viewbar=gtk_hbox_new(FALSE,5),*chat=gtk_hpaned_new(),*conversation=gtk_vbox_new(FALSE,0),*compose_outer=gtk_vbox_new(FALSE,4),*compose=gtk_hbox_new(FALSE,5),*meta=gtk_hbox_new(FALSE,8),*inspector=gtk_vbox_new(FALSE,7);
    GtkWidget *n=gtk_button_new_with_label("Новый чат"),*tmp=gtk_button_new_with_label("Временный"),*del=gtk_button_new_with_label("Удалить"),*settings=gtk_button_new_from_stock(GTK_STOCK_PREFERENCES),*emoji=gtk_button_new_with_label("😀"),*attach=gtk_button_new_with_label("▣"),*left_toggle,*right_toggle,*is,*label,*button;
    GtkListStore*store=gtk_list_store_new(2,G_TYPE_STRING,G_TYPE_UINT),*attachment_store=gtk_list_store_new(2,G_TYPE_STRING,G_TYPE_STRING);GtkCellRenderer*r=gtk_cell_renderer_text_new();GtkAccelGroup*accel=gtk_accel_group_new();guint i;
    const gchar *views[]={"Разговор","Агенты","Журнал","Изменения"};
    a->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(a->window),"Agent Desk — GTK2 AI Desktop");gtk_window_add_accel_group(GTK_WINDOW(a->window),accel);
    gtk_window_set_default_size(GTK_WINDOW(a->window),MAX(a->window_width,760),MAX(a->window_height,480));
    gtk_box_pack_start(GTK_BOX(v),build_menubar(a),FALSE,FALSE,0);
    gtk_container_set_border_width(GTK_CONTAINER(tools),5);gtk_box_pack_start(GTK_BOX(tools),n,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),tmp,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),del,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),gtk_vseparator_new(),FALSE,FALSE,2);
    left_toggle=gtk_toggle_button_new_with_label("Чаты");gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(left_toggle),a->left_panel_visible);gtk_box_pack_start(GTK_BOX(tools),left_toggle,FALSE,FALSE,0);
    right_toggle=gtk_toggle_button_new_with_label("Контекст");gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_toggle),a->right_panel_visible);gtk_box_pack_start(GTK_BOX(tools),right_toggle,FALSE,FALSE,0);
    gtk_box_pack_end(GTK_BOX(tools),settings,FALSE,FALSE,0);a->model_entry=gtk_entry_new();gtk_widget_set_size_request(a->model_entry,145,-1);gtk_entry_set_text(GTK_ENTRY(a->model_entry),!strcmp(a->provider,"openai")?a->openai_model:a->ollama_model);gtk_box_pack_end(GTK_BOX(tools),a->model_entry,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(tools),gtk_label_new("Модель:"),FALSE,FALSE,0);
    a->provider_combo=gtk_combo_box_new_text();gtk_combo_box_append_text(GTK_COMBO_BOX(a->provider_combo),"Ollama");gtk_combo_box_append_text(GTK_COMBO_BOX(a->provider_combo),"OpenAI");gtk_combo_box_set_active(GTK_COMBO_BOX(a->provider_combo),!strcmp(a->provider,"openai"));gtk_box_pack_end(GTK_BOX(tools),a->provider_combo,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(tools),gtk_label_new("Провайдер:"),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(v),tools,FALSE,FALSE,0);
    a->chat_list=gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));g_object_unref(store);gtk_tree_view_append_column(GTK_TREE_VIEW(a->chat_list),gtk_tree_view_column_new_with_attributes("Чаты",r,"text",0,NULL));gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->chat_list),FALSE);
    label=section_title("  ЧАТЫ");gtk_box_pack_start(GTK_BOX(sidebar),label,FALSE,FALSE,7);gtk_box_pack_start(GTK_BOX(sidebar),scroll(a->chat_list),TRUE,TRUE,0);gtk_widget_set_size_request(sidebar,205,-1);a->left_panel=sidebar;gtk_paned_pack1(GTK_PANED(paned),sidebar,FALSE,FALSE);
    for(i=0;i<4;i++){button=gtk_button_new_with_label(views[i]);g_object_set_data(G_OBJECT(button),"page",GINT_TO_POINTER(i));g_signal_connect(button,"clicked",G_CALLBACK(on_view),a);gtk_box_pack_start(GTK_BOX(viewbar),button,FALSE,FALSE,0);}gtk_container_set_border_width(GTK_CONTAINER(viewbar),5);gtk_box_pack_start(GTK_BOX(right),viewbar,FALSE,FALSE,0);
    a->notebook=gtk_notebook_new();gtk_notebook_set_show_tabs(GTK_NOTEBOOK(a->notebook),FALSE);gtk_notebook_set_show_border(GTK_NOTEBOOK(a->notebook),FALSE);
    a->transcript=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(a->transcript),FALSE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->transcript),GTK_WRAP_WORD_CHAR);gtk_text_view_set_left_margin(GTK_TEXT_VIEW(a->transcript),18);gtk_text_view_set_right_margin(GTK_TEXT_VIEW(a->transcript),18);gtk_box_pack_start(GTK_BOX(conversation),scroll(a->transcript),TRUE,TRUE,0);
    a->input=gtk_text_view_new();gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->input),GTK_WRAP_WORD_CHAR);is=scroll(a->input);gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(is),GTK_SHADOW_IN);gtk_widget_set_size_request(is,-1,64);
    gtk_box_pack_start(GTK_BOX(compose),attach,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(compose),emoji,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(compose),is,TRUE,TRUE,0);a->send=gtk_button_new_with_label("Отправить");gtk_box_pack_end(GTK_BOX(compose),a->send,FALSE,FALSE,0);
    a->attachment_count=gtk_label_new("▣ 0 влож.");a->agent_label=gtk_label_new("Агент: не выбран");gtk_label_set_ellipsize(GTK_LABEL(a->agent_label),PANGO_ELLIPSIZE_END);gtk_label_set_max_width_chars(GTK_LABEL(a->agent_label),24);
    a->mode_combo=gtk_combo_box_new_text();gtk_combo_box_append_text(GTK_COMBO_BOX(a->mode_combo),"Режим агента");gtk_combo_box_append_text(GTK_COMBO_BOX(a->mode_combo),"Режим диалога");gtk_combo_box_set_active(GTK_COMBO_BOX(a->mode_combo),0);
    gtk_box_pack_start(GTK_BOX(meta),a->mode_combo,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(meta),a->attachment_count,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(meta),a->agent_label,TRUE,TRUE,0);a->token_label=gtk_label_new("≈ 0 ток.");gtk_box_pack_end(GTK_BOX(meta),a->token_label,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(compose_outer),compose,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(compose_outer),meta,FALSE,FALSE,0);gtk_container_set_border_width(GTK_CONTAINER(compose_outer),7);gtk_box_pack_start(GTK_BOX(conversation),gtk_hseparator_new(),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(conversation),compose_outer,FALSE,FALSE,0);
    gtk_widget_set_size_request(inspector,250,-1);gtk_container_set_border_width(GTK_CONTAINER(inspector),9);gtk_box_pack_start(GTK_BOX(inspector),section_title("Контекст сеанса"),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),gtk_hseparator_new(),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),section_title("Активный агент"),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),left_label("Профиль выбирается на вкладке «Агенты»"),FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),section_title("Вложения"),FALSE,FALSE,5);
    a->attachments_list=gtk_tree_view_new_with_model(GTK_TREE_MODEL(attachment_store));g_object_unref(attachment_store);gtk_tree_view_append_column(GTK_TREE_VIEW(a->attachments_list),gtk_tree_view_column_new_with_attributes("Файл",r,"text",0,NULL));gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->attachments_list),FALSE);gtk_box_pack_start(GTK_BOX(inspector),scroll(a->attachments_list),TRUE,TRUE,0);button=gtk_button_new_with_label("Добавить файл…");g_signal_connect(button,"clicked",G_CALLBACK(on_attach),a);gtk_box_pack_start(GTK_BOX(inspector),button,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),section_title("Инструменты проекта"),FALSE,FALSE,5);
    a->agent_context=gtk_vbox_new(FALSE,7);
    a->project_label=left_label(a->project_root);gtk_label_set_ellipsize(GTK_LABEL(a->project_label),PANGO_ELLIPSIZE_MIDDLE);gtk_box_pack_start(GTK_BOX(a->agent_context),a->project_label,FALSE,FALSE,0);
    button=gtk_button_new_with_label("Выбрать папку проекта…");g_signal_connect(button,"clicked",G_CALLBACK(on_choose_project),a);gtk_box_pack_start(GTK_BOX(a->agent_context),button,FALSE,FALSE,0);
    a->permission_label=left_label("Разрешения: будут запрошены");gtk_label_set_line_wrap(GTK_LABEL(a->permission_label),TRUE);gtk_box_pack_start(GTK_BOX(a->agent_context),a->permission_label,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(a->agent_context),gtk_hseparator_new(),FALSE,FALSE,4);a->mcp_label=left_label("MCP: 0 серверов · 0 инструментов");gtk_box_pack_start(GTK_BOX(a->agent_context),a->mcp_label,FALSE,FALSE,0);
    button=gtk_button_new_with_label("Сбросить разрешения чата");g_signal_connect(button,"clicked",G_CALLBACK(on_reset_permissions),a);gtk_box_pack_start(GTK_BOX(a->agent_context),button,FALSE,FALSE,0);
    button=gtk_button_new_with_label("MCP и skills…");g_signal_connect(button,"clicked",G_CALLBACK(on_mcp_dialog),a);gtk_box_pack_start(GTK_BOX(a->agent_context),button,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(inspector),a->agent_context,FALSE,FALSE,0);update_mcp_label(a);
    gtk_paned_pack1(GTK_PANED(chat),conversation,TRUE,FALSE);a->right_panel=scroll(inspector);gtk_widget_set_size_request(a->right_panel,250,-1);gtk_paned_pack2(GTK_PANED(chat),a->right_panel,FALSE,FALSE);gtk_paned_set_position(GTK_PANED(chat),620);gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook),chat,NULL);gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook),build_agents_page(a),NULL);gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook),build_log_page(a),NULL);gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook),build_changes_page(a),NULL);gtk_box_pack_start(GTK_BOX(right),a->notebook,TRUE,TRUE,0);
    gtk_paned_pack2(GTK_PANED(paned),right,TRUE,FALSE);a->paned=paned;gtk_paned_set_position(GTK_PANED(paned),a->paned_position);gtk_box_pack_start(GTK_BOX(v),paned,TRUE,TRUE,0);
    {GtkWidget*statusbar=gtk_hbox_new(FALSE,8);gtk_container_set_border_width(GTK_CONTAINER(statusbar),4);a->status=gtk_label_new("● Готово");gtk_misc_set_alignment(GTK_MISC(a->status),0,0.5);a->cancel=gtk_button_new_with_label("Отменить запрос");gtk_widget_set_sensitive(a->cancel,FALSE);gtk_box_pack_start(GTK_BOX(statusbar),a->status,TRUE,TRUE,0);gtk_box_pack_end(GTK_BOX(statusbar),a->cancel,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(v),statusbar,FALSE,FALSE,0);}
    gtk_container_add(GTK_CONTAINER(a->window),v);gtk_widget_add_accelerator(a->send,"clicked",accel,GDK_Return,GDK_CONTROL_MASK,GTK_ACCEL_VISIBLE);g_object_unref(accel);
    g_signal_connect(a->window,"delete-event",G_CALLBACK(on_delete_window),a);g_signal_connect(a->window,"configure-event",G_CALLBACK(on_window_configure),a);g_signal_connect(a->paned,"notify::position",G_CALLBACK(on_paned_position),a);g_signal_connect(a->window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    g_signal_connect(n,"clicked",G_CALLBACK(on_new),a);g_signal_connect(tmp,"clicked",G_CALLBACK(on_temp),a);g_signal_connect(del,"clicked",G_CALLBACK(on_delete_chat),a);g_signal_connect(settings,"clicked",G_CALLBACK(on_settings),a);g_signal_connect(attach,"clicked",G_CALLBACK(on_attach),a);g_signal_connect(emoji,"clicked",G_CALLBACK(on_emoji),a);g_signal_connect(a->send,"clicked",G_CALLBACK(on_send),a);g_signal_connect(a->cancel,"clicked",G_CALLBACK(on_cancel),a);g_signal_connect(a->input,"key-press-event",G_CALLBACK(on_input_key_press),a);g_signal_connect(a->provider_combo,"changed",G_CALLBACK(on_provider_changed),a);g_signal_connect(a->model_entry,"changed",G_CALLBACK(on_model_changed),a);g_signal_connect(a->mode_combo,"changed",G_CALLBACK(on_mode_changed),a);
    g_signal_connect(left_toggle,"toggled",G_CALLBACK(on_left_panel_toggle),a);g_signal_connect(right_toggle,"toggled",G_CALLBACK(on_right_panel_toggle),a);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->chat_list)),"changed",G_CALLBACK(on_selection),a);gtk_widget_show_all(a->window);if(!a->left_panel_visible)gtk_widget_hide(a->left_panel);if(!a->right_panel_visible)gtk_widget_hide(a->right_panel);refresh_agents(a);
}

int main(int argc,char**argv){App a={0};if(!gtk_init_check(&argc,&argv)){g_printerr("Не удалось подключиться к графическому дисплею. Запустите программу из терминала рабочего стола или настройте DISPLAY/X11 forwarding.\n");return 1;}curl_global_init(CURL_GLOBAL_DEFAULT);a.project_root=g_canonicalize_filename(".",NULL);a.allow_read=TRUE;a.config_dir=g_build_filename(g_get_user_config_dir(),"gtk2aichat",NULL);g_mkdir_with_parents(a.config_dir,0700);a.history_path=config_file(&a,"history.json");a.settings_path=config_file(&a,"settings.conf");a.agents_path=config_file(&a,"agents.json");a.chats=g_ptr_array_new_with_free_func(chat_free);a.agents=g_ptr_array_new_with_free_func(agent_free);a.attachments=g_ptr_array_new_with_free_func(g_free);a.changes=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);load_settings(&a);if(!g_file_test(a.project_root,G_FILE_TEST_IS_DIR))set_str(&a.project_root,".");{gchar*canonical=g_canonicalize_filename(a.project_root,NULL);set_str(&a.project_root,canonical);g_free(canonical);}a.mcp=mcp_manager_new(a.project_root);load_agents(&a);load_history(&a);build_ui(&a);refresh_list(&a);if(!a.chats->len)new_chat(&a,FALSE);else select_chat(&a,g_ptr_array_index(a.chats,0));gtk_main();if(a.autosave_id)g_source_remove(a.autosave_id);save_history(&a);save_agents(&a);save_settings(&a);mcp_manager_free(a.mcp);g_hash_table_destroy(a.changes);g_ptr_array_free(a.attachments,TRUE);g_ptr_array_free(a.agents,TRUE);g_ptr_array_free(a.chats,TRUE);g_free(a.project_root);g_free(a.config_dir);g_free(a.history_path);g_free(a.settings_path);g_free(a.agents_path);g_free(a.provider);g_free(a.openai_url);g_free(a.openai_key);g_free(a.openai_model);g_free(a.ollama_url);g_free(a.ollama_model);g_free(a.emoji_font);g_free(a.user_color);g_free(a.assistant_color);curl_global_cleanup();return 0;}
