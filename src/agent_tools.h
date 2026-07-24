#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <glib.h>
#include <json-c/json.h>

json_object *agent_tools_schema(void);
gchar *agent_tool_execute(const gchar *name,
                          json_object *arguments,
                          const gchar *project_root,
                          gboolean allow_read,
                          gboolean allow_write);
gchar *agent_tool_file_diff(const gchar *project_root, const gchar *relative_path);

#endif
