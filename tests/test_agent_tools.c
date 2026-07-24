#include "../src/agent_tools.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static gchar *temporary_project(void) {
    gchar *template=g_build_filename(g_get_tmp_dir(),"gtk2aichat-tools-XXXXXX",NULL);
    g_assert_nonnull(g_mkdtemp(template));
    return template;
}

static json_object *args1(const gchar *key,const gchar *value) {
    json_object *args=json_object_new_object();
    json_object_object_add(args,key,json_object_new_string(value));
    return args;
}

static void test_path_sandbox(void) {
    gchar *root=temporary_project(),*result,*link=g_build_filename(root,"outside-link",NULL);
    json_object *args=args1("path","../outside");
    result=agent_tool_execute("read_file",args,root,TRUE,FALSE,FALSE);
    g_assert_true(g_str_has_prefix(result,"ERROR:"));
    g_free(result);json_object_put(args);
    g_assert_cmpint(symlink("/etc/passwd",link),==,0);
    args=args1("path","outside-link");result=agent_tool_execute("read_file",args,root,TRUE,FALSE,FALSE);
    g_assert_true(g_str_has_prefix(result,"ERROR:"));
    g_free(result);json_object_put(args);g_remove(link);g_rmdir(root);g_free(link);g_free(root);
}

static void test_read_and_search(void) {
    gchar *root=temporary_project(),*path=g_build_filename(root,"hello.txt",NULL),*result;
    json_object *args;
    g_assert_true(g_file_set_contents(path,"alpha\nneedle\nomega\n",-1,NULL));
    args=args1("path","hello.txt");result=agent_tool_execute("read_file",args,root,TRUE,FALSE,FALSE);
    g_assert_nonnull(strstr(result,"2: needle"));g_free(result);json_object_put(args);
    args=args1("query","needle");result=agent_tool_execute("search_files",args,root,TRUE,FALSE,FALSE);
    g_assert_nonnull(strstr(result,"hello.txt:2"));g_free(result);json_object_put(args);
    g_remove(path);g_rmdir(root);g_free(path);g_free(root);
}

static void test_write_permissions_and_replace(void) {
    gchar *root=temporary_project(),*path=g_build_filename(root,"edit.txt",NULL),*result,*contents=NULL;
    json_object *args=json_object_new_object();
    json_object_object_add(args,"path",json_object_new_string("edit.txt"));
    json_object_object_add(args,"content",json_object_new_string("before VALUE after"));
    result=agent_tool_execute("write_file",args,root,TRUE,FALSE,FALSE);
    g_assert_true(g_str_has_prefix(result,"ERROR:"));g_free(result);
    result=agent_tool_execute("write_file",args,root,TRUE,TRUE,FALSE);
    g_assert_true(g_str_has_prefix(result,"OK:"));g_free(result);json_object_put(args);
    args=json_object_new_object();
    json_object_object_add(args,"path",json_object_new_string("edit.txt"));
    json_object_object_add(args,"old_text",json_object_new_string("VALUE"));
    json_object_object_add(args,"new_text",json_object_new_string("changed"));
    result=agent_tool_execute("replace_in_file",args,root,TRUE,TRUE,FALSE);
    g_assert_true(g_str_has_prefix(result,"OK:"));g_free(result);json_object_put(args);
    g_assert_true(g_file_get_contents(path,&contents,NULL,NULL));
    g_assert_cmpstr(contents,==,"before changed after");
    g_free(contents);g_remove(path);g_rmdir(root);g_free(path);g_free(root);
}

static void test_schema(void) {
    json_object *schema=agent_tools_schema();
    g_assert_cmpuint(json_object_array_length(schema),==,8);
    json_object_put(schema);
}

static void test_skills(void) {
    gchar *root=temporary_project(),*directory=g_build_filename(root,".agents","skills","demo",NULL);
    gchar *file=g_build_filename(directory,"SKILL.md",NULL),*result;json_object *args;
    g_assert_cmpint(g_mkdir_with_parents(directory,0755),==,0);
    g_assert_true(g_file_set_contents(file,"---\nname: demo\ndescription: Demo skill\n---\nUse this instruction.\n",-1,NULL));
    args=json_object_new_object();result=agent_tool_execute("list_skills",args,root,TRUE,FALSE,FALSE);
    g_assert_nonnull(strstr(result,"demo"));g_assert_nonnull(strstr(result,"Demo skill"));g_free(result);json_object_put(args);
    args=args1("name","demo");result=agent_tool_execute("read_skill",args,root,TRUE,FALSE,FALSE);
    g_assert_nonnull(strstr(result,"Use this instruction."));g_free(result);json_object_put(args);
    g_remove(file);g_rmdir(directory);g_free(directory);
    directory=g_build_filename(root,".agents","skills",NULL);g_rmdir(directory);g_free(directory);
    directory=g_build_filename(root,".agents",NULL);g_rmdir(directory);g_free(directory);
    g_rmdir(root);g_free(file);g_free(root);
}

int main(int argc,char **argv) {
    g_test_init(&argc,&argv,NULL);
    g_test_add_func("/agent-tools/path-sandbox",test_path_sandbox);
    g_test_add_func("/agent-tools/read-search",test_read_and_search);
    g_test_add_func("/agent-tools/write-replace",test_write_permissions_and_replace);
    g_test_add_func("/agent-tools/schema",test_schema);
    g_test_add_func("/agent-tools/skills",test_skills);
    return g_test_run();
}
