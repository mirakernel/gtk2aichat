#include <gtk/gtk.h>
#include <curl/curl.h>
#include <json-c/json.h>
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
    GtkWidget *window, *chat_list, *transcript, *input, *send, *status;
    GPtrArray *chats;
    Chat *current;
    gchar *config_dir, *history_path, *settings_path;
    gchar *provider, *openai_url, *openai_key, *openai_model;
    gchar *ollama_url, *ollama_model;
    gboolean busy;
} App;

typedef struct { char *data; size_t len; } Buffer;
typedef struct { App *app; Chat *chat; gchar *request; } Job;
typedef struct { App *app; Chat *chat; gchar *answer; gchar *error; } Result;

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
    if (g_key_file_load_from_file(k, a->settings_path, G_KEY_FILE_NONE, &e)) {
#define GET(KEY, FIELD) do { gchar *v = g_key_file_get_string(k,"main",KEY,NULL); if(v){set_str(&a->FIELD,v);g_free(v);} } while(0)
        GET("provider", provider); GET("openai_url", openai_url); GET("openai_model", openai_model);
        if (!g_getenv("OPENAI_API_KEY")) GET("openai_key", openai_key);
        GET("ollama_url", ollama_url); GET("ollama_model", ollama_model);
#undef GET
    }
    if (e)
        g_error_free(e);
    g_key_file_free(k);
}
static void save_settings(App *a) {
    GKeyFile *k = g_key_file_new(); gsize n; gchar *data;
    g_key_file_set_string(k,"main","provider",a->provider); g_key_file_set_string(k,"main","openai_url",a->openai_url);
    g_key_file_set_string(k,"main","openai_model",a->openai_model); g_key_file_set_string(k,"main","openai_key",a->openai_key);
    g_key_file_set_string(k,"main","ollama_url",a->ollama_url); g_key_file_set_string(k,"main","ollama_model",a->ollama_model);
    data = g_key_file_to_data(k,&n,NULL); g_file_set_contents(a->settings_path,data,n,NULL); chmod(a->settings_path,0600);
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
    g_file_set_contents(a->history_path,json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN),-1,NULL); json_object_put(root);
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

static void refresh_transcript(App *a) {
    GtkTextBuffer *b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->transcript)); GString *s=g_string_new(""); guint i;
    if(a->current) for(i=0;i<a->current->messages->len;i++){ Message *m=g_ptr_array_index(a->current->messages,i);
        g_string_append_printf(s,"%s:\n%s\n\n",!strcmp(m->role,"user")?"Вы":"ИИ",m->text); }
    gtk_text_buffer_set_text(b,s->str,-1); g_string_free(s,TRUE);
    GtkTextIter end; gtk_text_buffer_get_end_iter(b,&end); gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(a->transcript),&end,0,FALSE,0,0);
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

static json_object *messages_json(Chat *c){ json_object *arr=json_object_new_array(),*o; guint i;
    if(c->system_prompt[0]){o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string("system"));json_object_object_add(o,"content",json_object_new_string(c->system_prompt));json_object_array_add(arr,o);}
    for(i=0;i<c->messages->len;i++){Message*m=g_ptr_array_index(c->messages,i);o=json_object_new_object();json_object_object_add(o,"role",json_object_new_string(m->role));json_object_object_add(o,"content",json_object_new_string(m->text));json_object_array_add(arr,o);} return arr;
}
static size_t write_cb(void *p,size_t s,size_t n,void *u){ Buffer*b=u; size_t z=s*n; char*q=realloc(b->data,b->len+z+1); if(!q)return 0;b->data=q;memcpy(q+b->len,p,z);b->len+=z;q[b->len]=0;return z; }
static gchar *extract_answer(const char *body,gboolean openai){ json_object*r=json_tokener_parse(body),*x,*y,*z; gchar*out=NULL;
    if(!r)
        return NULL;
    if(json_object_object_get_ex(r,"error",&x)){
        if(json_object_object_get_ex(x,"message",&y))out=g_strdup(json_object_get_string(y));
    }
    else if(openai&&json_object_object_get_ex(r,"choices",&x)&&json_object_array_length(x)>0){y=json_object_array_get_idx(x,0);if(json_object_object_get_ex(y,"message",&z)&&json_object_object_get_ex(z,"content",&x))out=g_strdup(json_object_get_string(x));}
    else if(!openai&&json_object_object_get_ex(r,"message",&x)&&json_object_object_get_ex(x,"content",&y))out=g_strdup(json_object_get_string(y));
    json_object_put(r);
    return out;
}
static gboolean deliver(gpointer p){ Result*r=p; App*a=r->app; if(r->answer)add_message(r->chat,"assistant",r->answer); else add_message(r->chat,"assistant",r->error?r->error:"Неизвестная ошибка"); if(a->current==r->chat)refresh_transcript(a);
    if(!r->chat->temporary)
        save_history(a);
    a->busy=FALSE;gtk_widget_set_sensitive(a->send,TRUE);gtk_label_set_text(GTK_LABEL(a->status),r->error?"Ошибка":"Готово");g_free(r->answer);g_free(r->error);g_free(r);return FALSE; }
static gpointer worker(gpointer p){ Job*j=p;App*a=j->app;gboolean oa=!strcmp(a->provider,"openai");CURL*c=curl_easy_init();Buffer b={calloc(1,1),0};struct curl_slist*h=NULL;Result*r=g_new0(Result,1);CURLcode rc;long status=0;
    r->app=a;r->chat=j->chat;h=curl_slist_append(h,"Content-Type: application/json");if(oa&&a->openai_key[0]){gchar*auth=g_strdup_printf("Authorization: Bearer %s",a->openai_key);h=curl_slist_append(h,auth);g_free(auth);}curl_easy_setopt(c,CURLOPT_URL,oa?a->openai_url:a->ollama_url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);curl_easy_setopt(c,CURLOPT_POSTFIELDS,j->request);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,&b);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(c,CURLOPT_TIMEOUT,180L);curl_easy_setopt(c,CURLOPT_USERAGENT,"gtk2aichat/0.1");rc=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    if(rc!=CURLE_OK)r->error=g_strdup_printf("Сетевая ошибка: %s",curl_easy_strerror(rc));else{r->answer=extract_answer(b.data,oa);if(!r->answer)r->error=g_strdup_printf("HTTP %ld: %.500s",status,b.data?b.data:"");}curl_slist_free_all(h);curl_easy_cleanup(c);free(b.data);g_free(j->request);g_free(j);g_idle_add(deliver,r);return NULL; }
static void on_send(GtkButton*b,gpointer data){(void)b;App*a=data;GtkTextBuffer*tb;GtkTextIter s,e;gchar*text;json_object*root;Job*j;if(a->busy||!a->current)return;tb=gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->input));gtk_text_buffer_get_bounds(tb,&s,&e);text=gtk_text_buffer_get_text(tb,&s,&e,FALSE);g_strstrip(text);if(!*text){g_free(text);return;}add_message(a->current,"user",text);if(a->current->messages->len==1&&!a->current->temporary){gchar*t=g_utf8_substring(text,0,MIN(36,g_utf8_strlen(text,-1)));set_str(&a->current->title,t);g_free(t);refresh_list(a);}gtk_text_buffer_set_text(tb,"",-1);refresh_transcript(a);if(!a->current->temporary)save_history(a);root=json_object_new_object();json_object_object_add(root,"model",json_object_new_string(!strcmp(a->provider,"openai")?a->openai_model:a->ollama_model));json_object_object_add(root,"messages",messages_json(a->current));if(strcmp(a->provider,"openai"))json_object_object_add(root,"stream",json_object_new_boolean(FALSE));j=g_new0(Job,1);j->app=a;j->chat=a->current;j->request=g_strdup(json_object_to_json_string_ext(root,JSON_C_TO_STRING_PLAIN));json_object_put(root);a->busy=TRUE;gtk_widget_set_sensitive(a->send,FALSE);gtk_label_set_text(GTK_LABEL(a->status),"Ожидание ответа…");g_thread_unref(g_thread_new("ai-request",worker,j));g_free(text);}

static GtkWidget *entry_row(GtkTable*t,int row,const char*label,const char*value){GtkWidget*l=gtk_label_new(label),*e=gtk_entry_new();gtk_misc_set_alignment(GTK_MISC(l),0,0.5);gtk_entry_set_text(GTK_ENTRY(e),value?value:"");gtk_table_attach(t,l,0,1,row,row+1,GTK_FILL,GTK_FILL,4,3);gtk_table_attach(t,e,1,2,row,row+1,GTK_EXPAND|GTK_FILL,GTK_FILL,4,3);return e;}
static void on_settings(GtkButton*b,gpointer data){(void)b;App*a=data;GtkWidget*d=gtk_dialog_new_with_buttons("Настройки",GTK_WINDOW(a->window),GTK_DIALOG_MODAL,GTK_STOCK_CANCEL,GTK_RESPONSE_CANCEL,GTK_STOCK_SAVE,GTK_RESPONSE_OK,NULL),*t=gtk_table_new(7,2,FALSE),*combo=gtk_combo_box_new_text(),*sys,*ou,*ok,*om,*lu,*lm;gtk_combo_box_append_text(GTK_COMBO_BOX(combo),"ollama");gtk_combo_box_append_text(GTK_COMBO_BOX(combo),"openai");gtk_combo_box_set_active(GTK_COMBO_BOX(combo),!strcmp(a->provider,"openai"));gtk_table_attach(GTK_TABLE(t),gtk_label_new("Провайдер"),0,1,0,1,GTK_FILL,GTK_FILL,4,3);gtk_table_attach(GTK_TABLE(t),combo,1,2,0,1,GTK_FILL,GTK_FILL,4,3);ou=entry_row(GTK_TABLE(t),1,"OpenAI URL",a->openai_url);ok=entry_row(GTK_TABLE(t),2,"OpenAI ключ",a->openai_key);gtk_entry_set_visibility(GTK_ENTRY(ok),FALSE);om=entry_row(GTK_TABLE(t),3,"OpenAI модель",a->openai_model);lu=entry_row(GTK_TABLE(t),4,"Ollama URL",a->ollama_url);lm=entry_row(GTK_TABLE(t),5,"Ollama модель",a->ollama_model);sys=entry_row(GTK_TABLE(t),6,"Системный промпт",a->current?a->current->system_prompt:"");gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox),t,TRUE,TRUE,6);gtk_widget_show_all(d);if(gtk_dialog_run(GTK_DIALOG(d))==GTK_RESPONSE_OK){set_str(&a->provider,gtk_combo_box_get_active(GTK_COMBO_BOX(combo))?"openai":"ollama");set_str(&a->openai_url,gtk_entry_get_text(GTK_ENTRY(ou)));set_str(&a->openai_key,gtk_entry_get_text(GTK_ENTRY(ok)));set_str(&a->openai_model,gtk_entry_get_text(GTK_ENTRY(om)));set_str(&a->ollama_url,gtk_entry_get_text(GTK_ENTRY(lu)));set_str(&a->ollama_model,gtk_entry_get_text(GTK_ENTRY(lm)));if(a->current)set_str(&a->current->system_prompt,gtk_entry_get_text(GTK_ENTRY(sys)));save_settings(a);save_history(a);}gtk_widget_destroy(d);}

static GtkWidget *scroll(GtkWidget*w){GtkWidget*s=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);gtk_container_add(GTK_CONTAINER(s),w);return s;}
static void build_ui(App*a){GtkWidget*v=gtk_vbox_new(FALSE,4),*tools=gtk_hbox_new(FALSE,4),*paned=gtk_hpaned_new(),*right=gtk_vbox_new(FALSE,4),*compose=gtk_hbox_new(FALSE,4),*n=gtk_button_new_with_label("Новый"),*tmp=gtk_button_new_with_label("Временный"),*settings=gtk_button_new_from_stock(GTK_STOCK_PREFERENCES);GtkListStore*store=gtk_list_store_new(2,G_TYPE_STRING,G_TYPE_UINT);GtkCellRenderer*r=gtk_cell_renderer_text_new();GtkTreeViewColumn*col=gtk_tree_view_column_new_with_attributes("Чаты",r,"text",0,NULL);a->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(a->window),"GTK2 AI Chat");gtk_window_set_default_size(GTK_WINDOW(a->window),900,560);gtk_container_set_border_width(GTK_CONTAINER(a->window),5);a->chat_list=gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));g_object_unref(store);gtk_tree_view_append_column(GTK_TREE_VIEW(a->chat_list),col);gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(a->chat_list),FALSE);a->transcript=gtk_text_view_new();gtk_text_view_set_editable(GTK_TEXT_VIEW(a->transcript),FALSE);gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->transcript),GTK_WRAP_WORD_CHAR);a->input=gtk_text_view_new();gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->input),GTK_WRAP_WORD_CHAR);a->send=gtk_button_new_with_label("Отправить");a->status=gtk_label_new("Готово");gtk_misc_set_alignment(GTK_MISC(a->status),0,0.5);gtk_box_pack_start(GTK_BOX(tools),n,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(tools),tmp,FALSE,FALSE,0);gtk_box_pack_end(GTK_BOX(tools),settings,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(v),tools,FALSE,FALSE,0);gtk_paned_pack1(GTK_PANED(paned),scroll(a->chat_list),FALSE,FALSE);gtk_box_pack_start(GTK_BOX(right),scroll(a->transcript),TRUE,TRUE,0);GtkWidget*is=scroll(a->input);gtk_widget_set_size_request(is,-1,85);gtk_box_pack_start(GTK_BOX(compose),is,TRUE,TRUE,0);gtk_box_pack_start(GTK_BOX(compose),a->send,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(right),compose,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(right),a->status,FALSE,FALSE,0);gtk_paned_pack2(GTK_PANED(paned),right,TRUE,FALSE);gtk_paned_set_position(GTK_PANED(paned),190);gtk_box_pack_start(GTK_BOX(v),paned,TRUE,TRUE,0);gtk_container_add(GTK_CONTAINER(a->window),v);g_signal_connect(a->window,"destroy",G_CALLBACK(gtk_main_quit),NULL);g_signal_connect(n,"clicked",G_CALLBACK(on_new),a);g_signal_connect(tmp,"clicked",G_CALLBACK(on_temp),a);g_signal_connect(settings,"clicked",G_CALLBACK(on_settings),a);g_signal_connect(a->send,"clicked",G_CALLBACK(on_send),a);g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(a->chat_list)),"changed",G_CALLBACK(on_selection),a);gtk_widget_show_all(a->window);}

int main(int argc,char**argv){App a={0};if(!gtk_init_check(&argc,&argv)){g_printerr("Не удалось подключиться к графическому дисплею. Запустите программу из терминала рабочего стола или настройте DISPLAY/X11 forwarding.\n");return 1;}curl_global_init(CURL_GLOBAL_DEFAULT);a.config_dir=g_build_filename(g_get_user_config_dir(),"gtk2aichat",NULL);g_mkdir_with_parents(a.config_dir,0700);a.history_path=config_file(&a,"history.json");a.settings_path=config_file(&a,"settings.conf");a.chats=g_ptr_array_new_with_free_func(chat_free);load_settings(&a);load_history(&a);build_ui(&a);refresh_list(&a);if(!a.chats->len)new_chat(&a,FALSE);else select_chat(&a,g_ptr_array_index(a.chats,0));gtk_main();save_history(&a);save_settings(&a);g_ptr_array_free(a.chats,TRUE);g_free(a.config_dir);g_free(a.history_path);g_free(a.settings_path);g_free(a.provider);g_free(a.openai_url);g_free(a.openai_key);g_free(a.openai_model);g_free(a.ollama_url);g_free(a.ollama_model);curl_global_cleanup();return 0;}
