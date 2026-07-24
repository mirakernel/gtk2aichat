#ifndef MCP_CLIENT_H
#define MCP_CLIENT_H

#include <glib.h>
#include <json-c/json.h>

typedef struct McpManager McpManager;

McpManager *mcp_manager_new(const gchar *project_root);
void mcp_manager_free(McpManager *manager);
gboolean mcp_manager_reload(McpManager *manager, gchar **error);
json_object *mcp_manager_tools_schema(McpManager *manager);
gboolean mcp_manager_has_tool(McpManager *manager, const gchar *name);
gchar *mcp_manager_call(McpManager *manager, const gchar *name, json_object *arguments);
guint mcp_manager_server_count(McpManager *manager);
guint mcp_manager_tool_count(McpManager *manager);
const gchar *mcp_manager_config_path(McpManager *manager);

#endif
