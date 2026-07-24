#include "mcp_client.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MCP_PROTOCOL_VERSION "2025-11-25"
#define MCP_MAX_MESSAGE (4 * 1024 * 1024)

typedef struct {
    gchar *name;
    gchar **argv;
    GPid pid;
    gint input_fd;
    gint output_fd;
    gint next_id;
    GPtrArray *tools;
    GMutex lock;
} McpServer;

typedef struct {
    gchar *public_name;
    gchar *remote_name;
    McpServer *server;
    json_object *schema;
} McpTool;

struct McpManager {
    gchar *project_root;
    gchar *config_path;
    GPtrArray *servers;
    GPtrArray *tools;
};

static void mcp_tool_free(gpointer data) {
    McpTool *tool=data;
    if(!tool)return;
    g_free(tool->public_name);g_free(tool->remote_name);
    if(tool->schema)json_object_put(tool->schema);
    g_free(tool);
}

static void mcp_server_free(gpointer data) {
    McpServer *server=data;
    if(!server)return;
    if(server->input_fd>=0)close(server->input_fd);
    if(server->output_fd>=0)close(server->output_fd);
    if(server->pid){
        pid_t waited;
        kill(server->pid,SIGTERM);
        waited=waitpid(server->pid,NULL,WNOHANG);
        if(waited==0){kill(server->pid,SIGKILL);waitpid(server->pid,NULL,0);}
        g_spawn_close_pid(server->pid);
    }
    g_strfreev(server->argv);g_free(server->name);
    if(server->tools)g_ptr_array_free(server->tools,TRUE);
    g_mutex_clear(&server->lock);g_free(server);
}

static gboolean write_all(gint fd,const gchar *data,gsize length) {
    while(length){
        ssize_t written=write(fd,data,length);
        if(written<0){if(errno==EINTR)continue;return FALSE;}
        data+=written;length-=written;
    }
    return TRUE;
}

static gchar *read_line(gint fd) {
    GString *line=g_string_new("");
    while(line->len<MCP_MAX_MESSAGE){
        struct pollfd wait_fd={fd,POLLIN,0};
        gchar byte;ssize_t count;
        if(poll(&wait_fd,1,15000)<=0){g_string_free(line,TRUE);return NULL;}
        count=read(fd,&byte,1);
        if(count==0){g_string_free(line,TRUE);return NULL;}
        if(count<0){if(errno==EINTR)continue;g_string_free(line,TRUE);return NULL;}
        if(byte=='\n')return g_string_free(line,FALSE);
        if(byte!='\r')g_string_append_c(line,byte);
    }
    g_string_free(line,TRUE);return NULL;
}

static json_object *rpc_request(McpServer *server,const gchar *method,json_object *params,gchar **error) {
    json_object *request=json_object_new_object(),*response=NULL,*id_value,*error_value,*message;
    gchar *line;gint id=server->next_id++;
    json_object_object_add(request,"jsonrpc",json_object_new_string("2.0"));
    json_object_object_add(request,"id",json_object_new_int(id));
    json_object_object_add(request,"method",json_object_new_string(method));
    if(params)json_object_object_add(request,"params",params);
    line=g_strconcat(json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN),"\n",NULL);
    if(!write_all(server->input_fd,line,strlen(line))){
        *error=g_strdup_printf("%s: cannot write JSON-RPC request",server->name);g_free(line);json_object_put(request);return NULL;
    }
    g_free(line);json_object_put(request);
    while((line=read_line(server->output_fd))!=NULL){
        response=json_tokener_parse(line);g_free(line);
        if(!response)continue;
        if(json_object_object_get_ex(response,"id",&id_value)&&json_object_get_int(id_value)==id)break;
        json_object_put(response);response=NULL;
    }
    if(!response){*error=g_strdup_printf("%s: MCP server closed stdout",server->name);return NULL;}
    if(json_object_object_get_ex(response,"error",&error_value)){
        if(json_object_object_get_ex(error_value,"message",&message))*error=g_strdup(json_object_get_string(message));
        else *error=g_strdup(json_object_to_json_string_ext(error_value,JSON_C_TO_STRING_PLAIN));
        json_object_put(response);return NULL;
    }
    return response;
}

static gboolean send_initialized(McpServer *server) {
    json_object *notification=json_object_new_object();
    gchar *line;gboolean ok;
    json_object_object_add(notification,"jsonrpc",json_object_new_string("2.0"));
    json_object_object_add(notification,"method",json_object_new_string("notifications/initialized"));
    line=g_strconcat(json_object_to_json_string_ext(notification,JSON_C_TO_STRING_PLAIN),"\n",NULL);
    ok=write_all(server->input_fd,line,strlen(line));
    g_free(line);json_object_put(notification);return ok;
}

static gchar *safe_name(const gchar *name) {
    GString *safe=g_string_new("");const gchar *p;
    for(p=name;*p;p++)g_string_append_c(safe,g_ascii_isalnum(*p)||*p=='_'?*p:'_');
    return g_string_free(safe,FALSE);
}

static gboolean initialize_server(McpManager *manager,McpServer *server,gchar **error) {
    json_object *params=json_object_new_object(),*capabilities=json_object_new_object(),*client=json_object_new_object();
    json_object *response,*result,*list_params,*tools_response,*tools_result,*tools,*cursor;gchar *server_safe;
    guint page=0;
    json_object_object_add(params,"protocolVersion",json_object_new_string(MCP_PROTOCOL_VERSION));
    json_object_object_add(params,"capabilities",capabilities);
    json_object_object_add(client,"name",json_object_new_string("gtk2aichat"));
    json_object_object_add(client,"title",json_object_new_string("Agent Desk"));
    json_object_object_add(client,"version",json_object_new_string("0.4"));
    json_object_object_add(params,"clientInfo",client);
    response=rpc_request(server,"initialize",params,error);
    if(!response)return FALSE;
    if(!json_object_object_get_ex(response,"result",&result)){json_object_put(response);*error=g_strdup("initialize response has no result");return FALSE;}
    json_object_put(response);
    if(!send_initialized(server)){*error=g_strdup("cannot send initialized notification");return FALSE;}
    server_safe=safe_name(server->name);
    list_params=json_object_new_object();
    do{
        tools_response=rpc_request(server,"tools/list",list_params,error);list_params=NULL;
        if(!tools_response){g_free(server_safe);return FALSE;}
        if(!json_object_object_get_ex(tools_response,"result",&tools_result)||!json_object_object_get_ex(tools_result,"tools",&tools)){
            json_object_put(tools_response);g_free(server_safe);*error=g_strdup("tools/list response has no tools");return FALSE;
        }
        {guint i;for(i=0;i<json_object_array_length(tools);i++){
            json_object *remote=json_object_array_get_idx(tools,i),*name_value,*description,*input_schema;
            McpTool *tool;gchar *tool_safe;
            if(!json_object_object_get_ex(remote,"name",&name_value))continue;
            tool=g_new0(McpTool,1);tool->server=server;tool->remote_name=g_strdup(json_object_get_string(name_value));
            tool_safe=safe_name(tool->remote_name);tool->public_name=g_strdup_printf("mcp__%s__%s",server_safe,tool_safe);g_free(tool_safe);
            tool->schema=json_object_new_object();json_object_object_add(tool->schema,"type",json_object_new_string("function"));
            {json_object *function=json_object_new_object();
             json_object_object_add(function,"name",json_object_new_string(tool->public_name));
             if(json_object_object_get_ex(remote,"description",&description))json_object_object_add(function,"description",json_object_get(description));
             else json_object_object_add(function,"description",json_object_new_string("MCP tool"));
             if(json_object_object_get_ex(remote,"inputSchema",&input_schema))json_object_object_add(function,"parameters",json_object_get(input_schema));
             else{json_object *empty=json_object_new_object();json_object_object_add(empty,"type",json_object_new_string("object"));json_object_object_add(function,"parameters",empty);}
             json_object_object_add(tool->schema,"function",function);}
            g_ptr_array_add(manager->tools,tool);g_ptr_array_add(server->tools,tool);
        }}
        if(json_object_object_get_ex(tools_result,"nextCursor",&cursor)&&json_object_is_type(cursor,json_type_string)&&*json_object_get_string(cursor)){
            list_params=json_object_new_object();json_object_object_add(list_params,"cursor",json_object_new_string(json_object_get_string(cursor)));
        }
        json_object_put(tools_response);page++;
    }while(list_params&&page<100);
    if(list_params)json_object_put(list_params);
    g_free(server_safe);return TRUE;
}

static McpServer *server_from_json(McpManager *manager,json_object *config,gchar **error) {
    json_object *name_value,*command_value,*args_value,*enabled_value;McpServer *server;guint count=0,i;GError *spawn_error=NULL;
    if(json_object_object_get_ex(config,"enabled",&enabled_value)&&!json_object_get_boolean(enabled_value))return NULL;
    if(!json_object_object_get_ex(config,"name",&name_value)||!json_object_object_get_ex(config,"command",&command_value)){
        *error=g_strdup("MCP server needs name and command");return NULL;
    }
    if(json_object_object_get_ex(config,"args",&args_value)&&json_object_is_type(args_value,json_type_array))count=json_object_array_length(args_value);
    server=g_new0(McpServer,1);server->input_fd=-1;server->output_fd=-1;server->next_id=1;
    server->name=g_strdup(json_object_get_string(name_value));server->argv=g_new0(gchar*,count+2);server->argv[0]=g_strdup(json_object_get_string(command_value));
    for(i=0;i<count;i++)server->argv[i+1]=g_strdup(json_object_get_string(json_object_array_get_idx(args_value,i)));
    server->tools=g_ptr_array_new();g_mutex_init(&server->lock);
    if(!g_spawn_async_with_pipes(manager->project_root,server->argv,NULL,G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,NULL,&server->pid,&server->input_fd,&server->output_fd,NULL,&spawn_error)){
        *error=g_strdup(spawn_error->message);g_error_free(spawn_error);mcp_server_free(server);return NULL;}
    if(!initialize_server(manager,server,error)){
        while(manager->tools->len){
            McpTool *tool=g_ptr_array_index(manager->tools,manager->tools->len-1);
            if(tool->server!=server)break;
            g_ptr_array_remove_index(manager->tools,manager->tools->len-1);
        }
        mcp_server_free(server);return NULL;
    }
    return server;
}

McpManager *mcp_manager_new(const gchar *project_root) {
    McpManager *manager=g_new0(McpManager,1);
    manager->project_root=g_strdup(project_root);
    manager->config_path=g_build_filename(project_root,".gtk2aichat-mcp.json",NULL);
    manager->servers=g_ptr_array_new_with_free_func(mcp_server_free);
    manager->tools=g_ptr_array_new_with_free_func(mcp_tool_free);
    return manager;
}

void mcp_manager_free(McpManager *manager) {
    if(!manager)return;
    g_ptr_array_free(manager->tools,TRUE);g_ptr_array_free(manager->servers,TRUE);
    g_free(manager->project_root);g_free(manager->config_path);g_free(manager);
}

gboolean mcp_manager_reload(McpManager *manager,gchar **error) {
    json_object *root,*servers;guint i;
    g_ptr_array_set_size(manager->tools,0);g_ptr_array_set_size(manager->servers,0);
    root=json_object_from_file(manager->config_path);
    if(!root){
        if(!g_file_test(manager->config_path,G_FILE_TEST_EXISTS))return TRUE;
        *error=g_strdup("cannot parse .gtk2aichat-mcp.json");return FALSE;
    }
    if(!json_object_object_get_ex(root,"servers",&servers)||!json_object_is_type(servers,json_type_array)){
        json_object_put(root);*error=g_strdup("MCP config needs a servers array");return FALSE;
    }
    for(i=0;i<json_object_array_length(servers);i++){
        McpServer *server=server_from_json(manager,json_object_array_get_idx(servers,i),error);
        if(*error){
            g_ptr_array_set_size(manager->tools,0);
            g_ptr_array_set_size(manager->servers,0);
            json_object_put(root);return FALSE;
        }
        if(server)g_ptr_array_add(manager->servers,server);
    }
    json_object_put(root);return TRUE;
}

json_object *mcp_manager_tools_schema(McpManager *manager) {
    json_object *tools=json_object_new_array();guint i;
    for(i=0;i<manager->tools->len;i++){McpTool*tool=g_ptr_array_index(manager->tools,i);json_object_array_add(tools,json_object_get(tool->schema));}
    return tools;
}

static McpTool *find_tool(McpManager *manager,const gchar *name) {
    guint i;for(i=0;i<manager->tools->len;i++){McpTool*tool=g_ptr_array_index(manager->tools,i);if(!strcmp(tool->public_name,name))return tool;}return NULL;
}
gboolean mcp_manager_has_tool(McpManager *manager,const gchar *name){return find_tool(manager,name)!=NULL;}

gchar *mcp_manager_call(McpManager *manager,const gchar *name,json_object *arguments) {
    McpTool *tool=find_tool(manager,name);json_object *params,*response,*result,*content,*is_error;gchar *error=NULL,*output;
    if(!tool)return g_strdup("ERROR: unknown MCP tool");
    params=json_object_new_object();json_object_object_add(params,"name",json_object_new_string(tool->remote_name));
    json_object_object_add(params,"arguments",json_object_get(arguments));
    g_mutex_lock(&tool->server->lock);response=rpc_request(tool->server,"tools/call",params,&error);g_mutex_unlock(&tool->server->lock);
    if(!response){output=g_strdup_printf("ERROR: %s",error?error:"MCP call failed");g_free(error);return output;}
    if(!json_object_object_get_ex(response,"result",&result)){json_object_put(response);return g_strdup("ERROR: MCP response has no result");}
    if(json_object_object_get_ex(result,"content",&content))output=g_strdup(json_object_to_json_string_ext(content,JSON_C_TO_STRING_PRETTY));
    else output=g_strdup(json_object_to_json_string_ext(result,JSON_C_TO_STRING_PRETTY));
    if(json_object_object_get_ex(result,"isError",&is_error)&&json_object_get_boolean(is_error)){
        gchar *marked=g_strconcat("ERROR: ",output,NULL);g_free(output);output=marked;
    }
    json_object_put(response);return output;
}

guint mcp_manager_server_count(McpManager *manager){return manager?manager->servers->len:0;}
guint mcp_manager_tool_count(McpManager *manager){return manager?manager->tools->len:0;}
const gchar *mcp_manager_config_path(McpManager *manager){return manager->config_path;}
