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

typedef struct {
    gchar *role;
    gchar *text;
} Message;

typedef struct {
    gchar *title;
    gchar *system_prompt;
    GPtrArray *messages;
    gboolean temporary;
} Chat;

typedef struct {
    GtkWidget *window, *chat_list, *transcript, *input, *send, *status, *paned;
    GPtrArray *chats;
    Chat *current;
    gchar *config_dir, *history_path, *settings_path;
    gchar *provider, *openai_url, *openai_key, *openai_model;
    gchar *ollama_url, *ollama_model;
    gchar *emoji_font;
    gchar *user_color, *assistant_color;
    gint window_width, window_height, paned_position;
    guint autosave_id;
    gboolean emoji_font_checked, emoji_font_available;
    gboolean busy;
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
typedef struct {
    Job *job;
    Buffer response;
    GString *pending;
    gboolean received_text;
} StreamContext;

static void message_free(gpointer p) {
    Message *m = p; if (!m) return; g_free(m->role); g_free(m->text); g_free(m);
}
static Chat *chat_new(gboolean temporary) {
    Chat *c = g_new0(Chat, 1); c->title = g_strdup(temporary ? "Временный чат" : "Новый чат");
    c->system_prompt = g_strdup("Ты полезный ассистент.");
    c->messages = g_ptr_array_new_with_free_func(message_free); c->temporary = temporary; return c;
}
static void chat_free(gpointer p) {
    Chat *c = p; if (!c) return; g_free(c->title); g_free(c->system_prompt);
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
    if (g_key_file_load_from_file(k, a->settings_path, G_KEY_FILE_NONE, &e)) {
#define GET(KEY, FIELD) do { gchar *v = g_key_file_get_string(k,"main",KEY,NULL); if(v){set_str(&a->FIELD,v);g_free(v);} } while(0)
        GET("provider", provider); GET("openai_url", openai_url); GET("openai_model", openai_model);
        if (!g_getenv("OPENAI_API_KEY")) GET("openai_key", openai_key);
        GET("ollama_url", ollama_url); GET("ollama_model", ollama_model);
        GET("emoji_font", emoji_font);
        GET("user_color", user_color); GET("assistant_color", assistant_color);
#undef GET
        a->window_width = g_key_file_get_integer(k,"window","width",NULL);
        a->window_height = g_key_file_get_integer(k,"window","height",NULL);
        a->paned_position = g_key_file_get_integer(k,"window","paned_position",NULL);
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
    g_key_file_set_string(k,"main","emoji_font",a->emoji_font);
    g_key_file_set_string(k,"main","user_color",a->user_color);g_key_file_set_string(k,"main","assistant_color",a->assistant_color);
    g_key_file_set_integer(k,"window","width",a->window_width);g_key_file_set_integer(k,"window","height",a->window_height);
    g_key_file_set_integer(k,"window","paned_position",a->paned_position);
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
        if(json_object_object_get_ex(o,"messages",&msgs)) for(j=0;j<json_object_array_length(msgs);j++){ json_object *mo=json_object_array_get_idx(msgs,j),*r,*t;
            if(json_object_object_get_ex(mo,"role",&r)&&json_object_object_get_ex(mo,"content",&t)) add_message(c,json_object_get_string(r),json_object_get_string(t)); }
        g_ptr_array_add(a->chats,c);
    } json_object_put(root);
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
}

static void insert_tagged(GtkTextBuffer *buffer, const gchar *text, gssize length, GtkTextTag *tag) {
    GtkTextIter start,end;
    gtk_text_buffer_get_end_iter(buffer,&start);
    gtk_text_buffer_insert(buffer,&start,text,length);
    if(tag){gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_apply_tag(buffer,tag,&start,&end);}
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

static void insert_markdown(GtkTextBuffer *buffer, const gchar *text) {
    gchar **lines=g_strsplit(text,"\n",-1);
    gboolean code_block=FALSE;
    guint i;
    for(i=0;lines[i];i++){
        const gchar *line=lines[i];
        GtkTextIter start,end;
        GtkTextTag *line_tag=NULL;
        if(g_str_has_prefix(line,"```")){code_block=!code_block;continue;}
        gtk_text_buffer_get_end_iter(buffer,&start);
        if(code_block){insert_tagged(buffer,line,-1,NULL);line_tag=text_tag(buffer,"codeblock");}
        else if(line[0]=='#'){
            while(*line=='#')line++;
            if(*line==' ')line++;
            insert_inline_markdown(buffer,line);line_tag=text_tag(buffer,"heading");
        }else if(g_str_has_prefix(line,"> ")){insert_inline_markdown(buffer,line+2);line_tag=text_tag(buffer,"quote");}
        else if(g_str_has_prefix(line,"- ")||g_str_has_prefix(line,"* ")){insert_tagged(buffer,"• ",-1,NULL);insert_inline_markdown(buffer,line+2);}
        else insert_inline_markdown(buffer,line);
        insert_tagged(buffer,"\n",-1,NULL);
        if(line_tag){gtk_text_buffer_get_end_iter(buffer,&end);gtk_text_buffer_apply_tag(buffer,line_tag,&start,&end);}
    }
    g_strfreev(lines);
}

static void refresh_transcript(App *a) {
    GtkTextBuffer *b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->transcript));GtkTextIter start,end;guint i;gchar*rendered;
    gtk_text_buffer_set_text(b,"",-1);ensure_chat_tags(a,b);
    if(a->current)for(i=0;i<a->current->messages->len;i++){
        Message*m=g_ptr_array_index(a->current->messages,i);gboolean user=!strcmp(m->role,"user");GtkTextTag*role=text_tag(b,user?"user":"assistant");
        gtk_text_buffer_get_end_iter(b,&start);insert_tagged(b,user?"Вы:\n":"ИИ:\n",-1,text_tag(b,"bold"));
        if(user)insert_tagged(b,m->text,-1,NULL);else insert_markdown(b,m->text);
        insert_tagged(b,"\n\n",-1,NULL);gtk_text_buffer_get_end_iter(b,&end);gtk_text_buffer_apply_tag(b,role,&start,&end);
    }
    gtk_text_buffer_get_bounds(b,&start,&end);rendered=gtk_text_buffer_get_text(b,&start,&end,FALSE);apply_emoji_font(a,b,rendered);g_free(rendered);
    gtk_text_buffer_get_end_iter(b,&end);gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(a->transcript),&end,0,FALSE,0,0);
}
static void refresh_list(App *a) {
    gtk_list_store_clear(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list)))); guint i;
    for(i=0;i<a->chats->len;i++){ Chat *c=g_ptr_array_index(a->chats,i); GtkTreeIter it;
        gtk_list_store_append(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list))),&it);
        gtk_list_store_set(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(a->chat_list))),&it,0,c->title,1,i,-1); }
}
static void select_chat(App *a, Chat *c) { a->current=c; refresh_transcript(a); }
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

static json_object *messages_json(Chat *c){ json_object *arr=json_object_new_array(),*o; guint i;
    if(c->system_prompt[0]){o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string("system"));json_object_object_add(o,"content",json_object_new_string(c->system_prompt));json_object_array_add(arr,o);}
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
    if (event->done) {
        schedule_autosave(a);
        a->busy = FALSE;
        gtk_widget_set_sensitive(a->send, TRUE);
        gtk_label_set_text(GTK_LABEL(a->status), event->error ? "Ошибка" : "Готово");
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

static gchar *parse_stream_line(Job *job, const gchar *line) {
    json_object *root, *a, *b, *c;
    const gchar *json = line;
    gchar *result = NULL;

    while (g_ascii_isspace(*json)) json++;
    if (job->openai) {
        if (!g_str_has_prefix(json, "data:"))
            return NULL;
        json += 5;
        while (g_ascii_isspace(*json)) json++;
        if (!strcmp(json, "[DONE]"))
            return NULL;
    }
    if (!*json)
        return NULL;

    root = json_tokener_parse(json);
    if (!root)
        return NULL;
    if (job->openai && json_object_object_get_ex(root,"choices",&a) && json_object_array_length(a)>0) {
        b=json_object_array_get_idx(a,0);
        if(json_object_object_get_ex(b,"delta",&c) && json_object_object_get_ex(c,"content",&a))
            result=g_strdup(json_object_get_string(a));
    } else if (!job->openai && json_object_object_get_ex(root,"message",&a) && json_object_object_get_ex(a,"content",&b)) {
        result=g_strdup(json_object_get_string(b));
    }
    json_object_put(root);
    return result;
}

static void process_complete_lines(StreamContext *ctx) {
    gchar *newline;
    GString *delta = g_string_new("");

    while ((newline = strchr(ctx->pending->str, '\n')) != NULL) {
        gsize length = (gsize)(newline - ctx->pending->str);
        gchar *line = g_strndup(ctx->pending->str, length);
        gchar *piece;
        if (length && line[length - 1] == '\r')
            line[length - 1] = '\0';
        piece = parse_stream_line(ctx->job, line);
        if (piece) {
            g_string_append(delta, piece);
            ctx->received_text = TRUE;
            g_free(piece);
        }
        g_free(line);
        g_string_erase(ctx->pending, 0, length + 1);
    }
    if (delta->len)
        queue_stream_event(ctx->job, delta->str, NULL, FALSE);
    g_string_free(delta, TRUE);
}

static size_t write_cb(void *p,size_t s,size_t n,void *u){
    StreamContext *ctx=u;
    size_t z=s*n;
    char*q=realloc(ctx->response.data,ctx->response.len+z+1);
    if(!q)return 0;
    ctx->response.data=q;
    memcpy(q+ctx->response.len,p,z);
    ctx->response.len+=z;
    q[ctx->response.len]=0;
    g_string_append_len(ctx->pending,p,z);
    process_complete_lines(ctx);
    return z;
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
static gpointer worker(gpointer p){ Job*j=p;CURL*c=curl_easy_init();StreamContext ctx={j,{calloc(1,1),0},g_string_new(""),FALSE};struct curl_slist*h=NULL;CURLcode rc=CURLE_FAILED_INIT;long status=0;gchar*error=NULL;
    if(c){h=curl_slist_append(h,"Content-Type: application/json");if(j->openai&&j->key[0]){gchar*auth=g_strdup_printf("Authorization: Bearer %s",j->key);h=curl_slist_append(h,auth);g_free(auth);}curl_easy_setopt(c,CURLOPT_URL,j->url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);curl_easy_setopt(c,CURLOPT_POSTFIELDS,j->request);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&ctx);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,180L);curl_easy_setopt(c,CURLOPT_USERAGENT,"gtk2aichat/0.2");rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);}
    if(ctx.pending->len){g_string_append_c(ctx.pending,'\n');process_complete_lines(&ctx);}
    if(rc!=CURLE_OK)error=g_strdup_printf("Сетевая ошибка: %s",curl_easy_strerror(rc));
    else if(status<200||status>=300){gchar*api=extract_answer(ctx.response.data?ctx.response.data:"",j->openai);error=api?api:g_strdup_printf("HTTP %ld: %.500s",status,ctx.response.data?ctx.response.data:"");}
    else if(!ctx.received_text)error=g_strdup("Сервер вернул пустой ответ");
    queue_stream_event(j,NULL,error,TRUE);g_free(error);curl_slist_free_all(h);if(c)curl_easy_cleanup(c);free(ctx.response.data);g_string_free(ctx.pending,TRUE);g_free(j->request);g_free(j->url);g_free(j->key);g_free(j);return NULL; }
static void on_send(GtkButton*b,gpointer data){
    App*a=data;GtkTextBuffer*tb;GtkTextIter s,e;gchar*text;json_object*root;Job*j;Message*answer;
    (void)b;
    if(a->busy||!a->current)return;
    tb=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->input));
    gtk_text_buffer_get_bounds(tb,&s,&e);
    text=gtk_text_buffer_get_text(tb,&s,&e,FALSE);
    g_strstrip(text);
    if(!*text){g_free(text);return;}
    add_message(a->current,"user",text);
    if(a->current->messages->len==1&&!a->current->temporary){
        gchar*t=g_utf8_substring(text,0,MIN(36,g_utf8_strlen(text,-1)));
        set_str(&a->current->title,t);g_free(t);refresh_list(a);
    }
    root=json_object_new_object();
    json_object_object_add(root,"model",json_object_new_string(!strcmp(a->provider,"openai")?a->openai_model:a->ollama_model));
    json_object_object_add(root,"messages",messages_json(a->current));
    json_object_object_add(root,"stream",json_object_new_boolean(TRUE));
    j=g_new0(Job,1);j->app=a;j->chat=a->current;j->openai=!strcmp(a->provider,"openai");
    j->url=g_strdup(j->openai?a->openai_url:a->ollama_url);j->key=g_strdup(a->openai_key);
    j->request=g_strdup(json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    add_message(a->current,"assistant","");
    answer=g_ptr_array_index(a->current->messages,a->current->messages->len-1);
    j->answer=answer;
    gtk_text_buffer_set_text(tb,"",-1);refresh_transcript(a);
    if(!a->current->temporary)save_history(a);
    a->busy=TRUE;gtk_widget_set_sensitive(a->send,FALSE);
    gtk_label_set_text(GTK_LABEL(a->status),"Получение ответа…");
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
static void build_ui(App*a){
    GtkWidget*v=gtk_vbox_new(FALSE,4),*tools=gtk_hbox_new(FALSE,4),*paned=gtk_hpaned_new();
    GtkWidget*right=gtk_vbox_new(FALSE,4),*compose=gtk_hbox_new(FALSE,4);
    GtkWidget*n=gtk_button_new_with_label("Новый"),*tmp=gtk_button_new_with_label("Временный"),*del=gtk_button_new_with_label("Удалить");
    GtkWidget*settings=gtk_button_new_from_stock(GTK_STOCK_PREFERENCES),*emoji=gtk_button_new_with_label("😀");
    GtkListStore*store=gtk_list_store_new(2,G_TYPE_STRING,G_TYPE_UINT);GtkCellRenderer*r=gtk_cell_renderer_text_new();GtkAccelGroup*accel=gtk_accel_group_new();
    GtkTreeViewColumn*col=gtk_tree_view_column_new_with_attributes("Чаты",r,"text",0,NULL);GtkWidget*is;
    a->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(a->window),"GTK2 AI Chat");gtk_window_add_accel_group(GTK_WINDOW(a->window),accel);
    gtk_window_set_default_size(GTK_WINDOW(a->window),a->window_width,a->window_height);gtk_container_set_border_width(GTK_CONTAINER(a->window),5);
    a->chat_list=gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));g_object_unref(store);
    gtk_tree_view_append_column(GTK_TREE_VIEW(a->chat_list),col);gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->chat_list),FALSE);
    a->transcript=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(a->transcript),FALSE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->transcript),GTK_WRAP_WORD_CHAR);
    a->input=gtk_text_view_new();gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->input),GTK_WRAP_WORD_CHAR);
    a->send=gtk_button_new_with_label("Отправить");gtk_widget_add_accelerator(a->send,"clicked",accel,GDK_Return,0,GTK_ACCEL_VISIBLE);gtk_widget_add_accelerator(a->send,"clicked",accel,GDK_KP_Enter,0,GTK_ACCEL_VISIBLE);g_object_unref(accel);a->status=gtk_label_new("Готово");gtk_misc_set_alignment(GTK_MISC(a->status),0,0.5);
    gtk_box_pack_start(GTK_BOX(tools),n,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),tmp,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),del,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(tools),settings,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(v),tools,FALSE,FALSE,0);
    gtk_paned_pack1(GTK_PANED(paned),scroll(a->chat_list),FALSE,FALSE);gtk_box_pack_start(GTK_BOX(right),scroll(a->transcript),TRUE,TRUE,0);
    is=scroll(a->input);gtk_widget_set_size_request(is,-1,85);gtk_box_pack_start(GTK_BOX(compose),is,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(compose),emoji,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(compose),a->send,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(right),compose,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(right),a->status,FALSE,FALSE,0);
    a->paned=paned;gtk_paned_pack2(GTK_PANED(paned),right,TRUE,FALSE);gtk_paned_set_position(GTK_PANED(paned),a->paned_position);gtk_box_pack_start(GTK_BOX(v),paned,TRUE,TRUE,0);gtk_container_add(GTK_CONTAINER(a->window),v);
    g_signal_connect(a->window,"delete-event",G_CALLBACK(on_delete_window),a);g_signal_connect(a->window,"configure-event",G_CALLBACK(on_window_configure),a);g_signal_connect(a->paned,"notify::position",G_CALLBACK(on_paned_position),a);g_signal_connect(a->window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
    g_signal_connect(n,"clicked",G_CALLBACK(on_new),a);g_signal_connect(tmp,"clicked",G_CALLBACK(on_temp),a);g_signal_connect(del,"clicked",G_CALLBACK(on_delete_chat),a);g_signal_connect(settings,"clicked",G_CALLBACK(on_settings),a);
    g_signal_connect(emoji,"clicked",G_CALLBACK(on_emoji),a);g_signal_connect(a->send,"clicked",G_CALLBACK(on_send),a);g_signal_connect(a->input,"key-press-event",G_CALLBACK(on_input_key_press),a);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->chat_list)),"changed",G_CALLBACK(on_selection),a);gtk_widget_show_all(a->window);
}

int main(int argc,char**argv){App a={0};if(!gtk_init_check(&argc,&argv)){g_printerr("Не удалось подключиться к графическому дисплею. Запустите программу из терминала рабочего стола или настройте DISPLAY/X11 forwarding.\n");return 1;}curl_global_init(CURL_GLOBAL_DEFAULT);a.config_dir=g_build_filename(g_get_user_config_dir(),"gtk2aichat",NULL);g_mkdir_with_parents(a.config_dir,0700);a.history_path=config_file(&a,"history.json");a.settings_path=config_file(&a,"settings.conf");a.chats=g_ptr_array_new_with_free_func(chat_free);load_settings(&a);load_history(&a);build_ui(&a);refresh_list(&a);if(!a.chats->len)new_chat(&a,FALSE);else select_chat(&a,g_ptr_array_index(a.chats,0));gtk_main();if(a.autosave_id)g_source_remove(a.autosave_id);save_history(&a);save_settings(&a);g_ptr_array_free(a.chats,TRUE);g_free(a.config_dir);g_free(a.history_path);g_free(a.settings_path);g_free(a.provider);g_free(a.openai_url);g_free(a.openai_key);g_free(a.openai_model);g_free(a.ollama_url);g_free(a.ollama_model);g_free(a.emoji_font);g_free(a.user_color);g_free(a.assistant_color);curl_global_cleanup();return 0;}
