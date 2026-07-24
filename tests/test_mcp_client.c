#include "../src/mcp_client.h"

#include <glib/gstdio.h>
#include <string.h>

int main(int argc,char **argv) {
    gchar *root,*template,*config,*server_path,*json,*error=NULL,*result;
    McpManager *manager;json_object *args,*schema;
    g_test_init(&argc,&argv,NULL);
    g_assert_cmpint(argc,==,2);
    template=g_build_filename(g_get_tmp_dir(),"gtk2aichat-mcp-XXXXXX",NULL);
    root=g_mkdtemp(template);g_assert_nonnull(root);
    server_path=g_canonicalize_filename(argv[1],NULL);
    config=g_build_filename(root,".gtk2aichat-mcp.json",NULL);
    json=g_strdup_printf("{\"servers\":[{\"name\":\"test-server\",\"command\":\"%s\",\"args\":[]}]}",server_path);
    g_assert_true(g_file_set_contents(config,json,-1,NULL));
    manager=mcp_manager_new(root);
    g_assert_true(mcp_manager_reload(manager,&error));
    g_assert_null(error);
    g_assert_cmpuint(mcp_manager_server_count(manager),==,1);
    g_assert_cmpuint(mcp_manager_tool_count(manager),==,1);
    schema=mcp_manager_tools_schema(manager);g_assert_cmpuint(json_object_array_length(schema),==,1);json_object_put(schema);
    args=json_object_new_object();result=mcp_manager_call(manager,"mcp__test_server__echo",args);
    g_assert_nonnull(strstr(result,"echo-ok"));
    g_free(result);json_object_put(args);mcp_manager_free(manager);
    g_remove(config);g_rmdir(root);g_free(json);g_free(config);g_free(server_path);g_free(template);
    return 0;
}
