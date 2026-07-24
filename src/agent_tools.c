#include "agent_tools.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define MAX_TOOL_OUTPUT (256 * 1024)
#define MAX_FILE_SIZE (1024 * 1024)

static json_object *property(const gchar *type, const gchar *description) {
    json_object *value=json_object_new_object();
    json_object_object_add(value,"type",json_object_new_string(type));
    json_object_object_add(value,"description",json_object_new_string(description));
    return value;
}

static void add_tool(json_object *tools, const gchar *name, const gchar *description,
                     json_object *properties, const gchar **required) {
    json_object *tool=json_object_new_object(),*function=json_object_new_object();
    json_object *parameters=json_object_new_object(),*required_array=json_object_new_array();
    guint i;
    json_object_object_add(parameters,"type",json_object_new_string("object"));
    json_object_object_add(parameters,"properties",properties);
    for(i=0;required&&required[i];i++)
        json_object_array_add(required_array,json_object_new_string(required[i]));
    json_object_object_add(parameters,"required",required_array);
    json_object_object_add(parameters,"additionalProperties",json_object_new_boolean(FALSE));
    json_object_object_add(function,"name",json_object_new_string(name));
    json_object_object_add(function,"description",json_object_new_string(description));
    json_object_object_add(function,"parameters",parameters);
    json_object_object_add(tool,"type",json_object_new_string("function"));
    json_object_object_add(tool,"function",function);
    json_object_array_add(tools,tool);
}

json_object *agent_tools_schema(void) {
    json_object *tools=json_object_new_array(),*p;
    static const gchar *path_required[]={"path",NULL};
    static const gchar *search_required[]={"query",NULL};
    static const gchar *write_required[]={"path","content",NULL};
    static const gchar *replace_required[]={"path","old_text","new_text",NULL};
    static const gchar *build_required[]={"command",NULL};

    p=json_object_new_object();
    json_object_object_add(p,"path",property("string","Directory relative to the project root; use . for the root."));
    add_tool(tools,"list_files","List files and directories inside the project. Results are recursive and exclude .git.",p,path_required);
    p=json_object_new_object();
    json_object_object_add(p,"path",property("string","File path relative to the project root."));
    add_tool(tools,"read_file","Read a UTF-8 text file, with line numbers. Files are limited to 1 MiB.",p,path_required);
    p=json_object_new_object();
    json_object_object_add(p,"query",property("string","Literal UTF-8 text to search for."));
    json_object_object_add(p,"path",property("string","Optional directory relative to the project root; defaults to ."));
    add_tool(tools,"search_files","Search text files recursively and return matching file names and line numbers.",p,search_required);
    p=json_object_new_object();
    json_object_object_add(p,"path",property("string","File path relative to the project root."));
    json_object_object_add(p,"content",property("string","Complete new UTF-8 contents of the file."));
    add_tool(tools,"write_file","Create or completely replace a project text file. Requires write permission.",p,write_required);
    p=json_object_new_object();
    json_object_object_add(p,"path",property("string","File path relative to the project root."));
    json_object_object_add(p,"old_text",property("string","Exact text that must occur exactly once."));
    json_object_object_add(p,"new_text",property("string","Replacement text."));
    add_tool(tools,"replace_in_file","Replace one exact, unique text fragment. Requires write permission.",p,replace_required);
    p=json_object_new_object();
    {json_object *command=property("string","Allowed command to run in the project root.");
     json_object *values=json_object_new_array();
     json_object_array_add(values,json_object_new_string("make"));
     json_object_array_add(values,json_object_new_string("make clean"));
     json_object_array_add(values,json_object_new_string("git diff --check"));
     json_object_array_add(values,json_object_new_string("desktop-file-validate gtk2aichat.desktop"));
     json_object_object_add(command,"enum",values);json_object_object_add(p,"command",command);}
    add_tool(tools,"run_build","Run one allowlisted build or validation command without a shell. Requires build permission.",p,build_required);
    return tools;
}

static gchar *error_result(const gchar *format, ...) {
    va_list ap;
    gchar *detail,*result;
    va_start(ap,format);detail=g_strdup_vprintf(format,ap);va_end(ap);
    result=g_strdup_printf("ERROR: %s",detail);g_free(detail);
    return result;
}

static const gchar *arg_string(json_object *args, const gchar *key) {
    json_object *value;
    if(!args||!json_object_object_get_ex(args,key,&value)||!json_object_is_type(value,json_type_string))
        return NULL;
    return json_object_get_string(value);
}

static gchar *safe_path(const gchar *root, const gchar *relative, gboolean may_not_exist) {
    gchar *joined,*canonical,*parent,*resolved_parent,*resolved_root,*resolved_existing;
    gsize root_len;
    if(!relative||g_path_is_absolute(relative))return NULL;
    resolved_root=realpath(root,NULL);
    if(!resolved_root)return NULL;
    joined=g_build_filename(root,relative,NULL);
    canonical=g_canonicalize_filename(joined,NULL);g_free(joined);
    root_len=strlen(resolved_root);
    if(strncmp(canonical,resolved_root,root_len)||
       (canonical[root_len]&&canonical[root_len]!=G_DIR_SEPARATOR)){
        g_free(canonical);free(resolved_root);return NULL;
    }
    if(g_file_test(canonical,G_FILE_TEST_EXISTS)){
        resolved_existing=realpath(canonical,NULL);g_free(canonical);
        if(!resolved_existing||strncmp(resolved_existing,resolved_root,root_len)||
           (resolved_existing[root_len]&&resolved_existing[root_len]!=G_DIR_SEPARATOR)){
            free(resolved_existing);free(resolved_root);return NULL;
        }
        canonical=g_strdup(resolved_existing);free(resolved_existing);free(resolved_root);return canonical;
    }
    if(!may_not_exist){g_free(canonical);free(resolved_root);return NULL;}
    parent=g_path_get_dirname(canonical);
    while(!g_file_test(parent,G_FILE_TEST_EXISTS)){
        gchar *next=g_path_get_dirname(parent);
        if(!strcmp(next,parent)){g_free(next);break;}
        g_free(parent);parent=next;
    }
    resolved_parent=realpath(parent,NULL);g_free(parent);
    if(!resolved_parent||strncmp(resolved_parent,resolved_root,root_len)||
       (resolved_parent[root_len]&&resolved_parent[root_len]!=G_DIR_SEPARATOR)){
        free(resolved_parent);free(resolved_root);g_free(canonical);return NULL;
    }
    free(resolved_parent);free(resolved_root);return canonical;
}

static void list_recursive(const gchar *root, const gchar *path, GString *out, guint depth) {
    GDir *dir;const gchar *name;GError *error=NULL;
    if(depth>12||out->len>=MAX_TOOL_OUTPUT)return;
    dir=g_dir_open(path,0,&error);
    if(!dir){g_string_append_printf(out,"[cannot open: %s]\n",error->message);g_error_free(error);return;}
    while((name=g_dir_read_name(dir))!=NULL&&out->len<MAX_TOOL_OUTPUT){
        gchar *full,*relative;
        if(!strcmp(name,".git"))continue;
        full=g_build_filename(path,name,NULL);relative=g_filename_to_utf8(full+strlen(root)+1,-1,NULL,NULL,NULL);
        g_string_append_printf(out,"%s%s\n",relative?relative:name,g_file_test(full,G_FILE_TEST_IS_SYMLINK)?"@":(g_file_test(full,G_FILE_TEST_IS_DIR)?"/":""));
        if(!g_file_test(full,G_FILE_TEST_IS_SYMLINK)&&g_file_test(full,G_FILE_TEST_IS_DIR))list_recursive(root,full,out,depth+1);
        g_free(relative);g_free(full);
    }
    g_dir_close(dir);
}

static gchar *read_file_tool(const gchar *path) {
    gchar *data=NULL;gsize length=0;GError *error=NULL;GString *out;gchar **lines;guint i;
    if(!g_file_get_contents(path,&data,&length,&error)){
        gchar *result=error_result("%s",error->message);g_error_free(error);return result;
    }
    if(length>MAX_FILE_SIZE){g_free(data);return error_result("file exceeds 1 MiB");}
    if(!g_utf8_validate(data,length,NULL)){g_free(data);return error_result("file is not valid UTF-8 text");}
    lines=g_strsplit(data,"\n",-1);out=g_string_new("");
    for(i=0;lines[i]&&out->len<MAX_TOOL_OUTPUT;i++)g_string_append_printf(out,"%u: %s\n",i+1,lines[i]);
    if(out->len>=MAX_TOOL_OUTPUT)g_string_append(out,"[output truncated]\n");
    g_strfreev(lines);g_free(data);return g_string_free(out,FALSE);
}

static void search_recursive(const gchar *root,const gchar *path,const gchar *query,GString*out,guint depth){
    GDir*dir;const gchar*name;
    if(depth>12||out->len>=MAX_TOOL_OUTPUT)return;
    dir=g_dir_open(path,0,NULL);if(!dir)return;
    while((name=g_dir_read_name(dir))&&out->len<MAX_TOOL_OUTPUT){
        gchar*full=g_build_filename(path,name,NULL);
        if(!strcmp(name,".git")){g_free(full);continue;}
        if(g_file_test(full,G_FILE_TEST_IS_SYMLINK)){g_free(full);continue;}
        if(g_file_test(full,G_FILE_TEST_IS_DIR))search_recursive(root,full,query,out,depth+1);
        else{gchar*data=NULL;gsize length=0;if(g_file_get_contents(full,&data,&length,NULL)&&length<=MAX_FILE_SIZE&&g_utf8_validate(data,length,NULL)){
            gchar**lines=g_strsplit(data,"\n",-1);guint i;for(i=0;lines[i]&&out->len<MAX_TOOL_OUTPUT;i++)if(strstr(lines[i],query))g_string_append_printf(out,"%s:%u: %s\n",full+strlen(root)+1,i+1,lines[i]);g_strfreev(lines);}g_free(data);}
        g_free(full);
    }g_dir_close(dir);
}

static gchar *write_atomic(const gchar *path,const gchar *content){
    gchar *parent=g_path_get_dirname(path),*tmp;GError*error=NULL;gboolean ok;
    if(g_mkdir_with_parents(parent,0755)!=0){gchar*r=error_result("cannot create parent directory: %s",g_strerror(errno));g_free(parent);return r;}
    g_free(parent);tmp=g_strconcat(path,".agent-tmp",NULL);
    ok=g_file_set_contents(tmp,content,-1,&error)&&g_rename(tmp,path)==0;
    if(!ok){gchar*r=error_result("%s",error?error->message:g_strerror(errno));if(error)g_error_free(error);g_unlink(tmp);g_free(tmp);return r;}
    g_free(tmp);return g_strdup("OK: file written atomically");
}

static gchar *run_command(const gchar *root,const gchar *command){
    gchar **argv=NULL,*stdout_data=NULL,*stderr_data=NULL;gint status=0;GError*error=NULL;GString*out;
    if(!strcmp(command,"make")){argv=g_new0(gchar*,2);argv[0]=g_strdup("make");}
    else if(!strcmp(command,"make clean")){argv=g_new0(gchar*,3);argv[0]=g_strdup("make");argv[1]=g_strdup("clean");}
    else if(!strcmp(command,"git diff --check")){argv=g_new0(gchar*,4);argv[0]=g_strdup("git");argv[1]=g_strdup("diff");argv[2]=g_strdup("--check");}
    else if(!strcmp(command,"desktop-file-validate gtk2aichat.desktop")){argv=g_new0(gchar*,3);argv[0]=g_strdup("desktop-file-validate");argv[1]=g_strdup("gtk2aichat.desktop");}
    else return error_result("command is not allowlisted");
    if(!g_spawn_sync(root,argv,NULL,G_SPAWN_SEARCH_PATH,NULL,NULL,&stdout_data,&stderr_data,&status,&error)){
        gchar*r=error_result("%s",error->message);g_error_free(error);g_strfreev(argv);return r;}
    out=g_string_new("");if(stdout_data)g_string_append(out,stdout_data);if(stderr_data)g_string_append(out,stderr_data);
    if(WIFEXITED(status)){gchar*prefix=g_strdup_printf("exit code: %d\n",WEXITSTATUS(status));g_string_prepend(out,prefix);g_free(prefix);}
    else g_string_prepend(out,"process terminated abnormally\n");
    if(out->len>MAX_TOOL_OUTPUT)g_string_truncate(out,MAX_TOOL_OUTPUT);
    g_free(stdout_data);g_free(stderr_data);g_strfreev(argv);return g_string_free(out,FALSE);
}

gchar *agent_tool_execute(const gchar *name,json_object *arguments,const gchar *project_root,
                          gboolean allow_read,gboolean allow_write,gboolean allow_build){
    const gchar *path_arg,*query,*content,*old_text,*new_text,*command;gchar*path,*data,*result;GString*out;
    if(!name||!arguments)return error_result("invalid tool call");
    if((!strcmp(name,"list_files")||!strcmp(name,"read_file")||!strcmp(name,"search_files"))&&!allow_read)
        return error_result("read permission is disabled");
    if(!strcmp(name,"list_files")){
        path_arg=arg_string(arguments,"path");path=safe_path(project_root,path_arg,FALSE);if(!path)return error_result("path is outside the project");
        if(!g_file_test(path,G_FILE_TEST_IS_DIR)){g_free(path);return error_result("path is not a directory");}
        out=g_string_new("");list_recursive(project_root,path,out,0);g_free(path);return g_string_free(out,FALSE);
    }
    if(!strcmp(name,"read_file")){
        path_arg=arg_string(arguments,"path");path=safe_path(project_root,path_arg,FALSE);if(!path)return error_result("path is outside the project");
        result=read_file_tool(path);g_free(path);return result;
    }
    if(!strcmp(name,"search_files")){
        query=arg_string(arguments,"query");path_arg=arg_string(arguments,"path");if(!path_arg)path_arg=".";
        if(!query||!*query)return error_result("query is required");
        path=safe_path(project_root,path_arg,FALSE);if(!path)return error_result("path is outside the project");
        out=g_string_new("");search_recursive(project_root,path,query,out,0);g_free(path);if(!out->len)g_string_append(out,"No matches.");return g_string_free(out,FALSE);
    }
    if(!strcmp(name,"write_file")){
        if(!allow_write)return error_result("write permission is disabled");
        path_arg=arg_string(arguments,"path");content=arg_string(arguments,"content");if(!content)return error_result("content is required");
        path=safe_path(project_root,path_arg,TRUE);if(!path)return error_result("path is outside the project");
        result=write_atomic(path,content);g_free(path);return result;
    }
    if(!strcmp(name,"replace_in_file")){
        if(!allow_write)return error_result("write permission is disabled");
        path_arg=arg_string(arguments,"path");old_text=arg_string(arguments,"old_text");new_text=arg_string(arguments,"new_text");
        path=safe_path(project_root,path_arg,FALSE);if(!path||!old_text||!*old_text||!new_text){g_free(path);return error_result("invalid replacement arguments");}
        if(!g_file_get_contents(path,&data,NULL,NULL)){g_free(path);return error_result("cannot read file");}
        {gchar*first=strstr(data,old_text),*second=first?strstr(first+strlen(old_text),old_text):NULL;
         if(!first||second){g_free(data);g_free(path);return error_result("old_text must occur exactly once");}
         out=g_string_new_len(data,first-data);g_string_append(out,new_text);g_string_append(out,first+strlen(old_text));result=write_atomic(path,out->str);g_string_free(out,TRUE);}
        g_free(data);g_free(path);return result;
    }
    if(!strcmp(name,"run_build")){
        if(!allow_build)return error_result("build permission is disabled");
        command=arg_string(arguments,"command");if(!command)return error_result("command is required");return run_command(project_root,command);
    }
    return error_result("unknown tool: %s",name);
}
