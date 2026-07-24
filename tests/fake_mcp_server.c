#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char line[65536];
    while(fgets(line,sizeof(line),stdin)){
        json_object *request=json_tokener_parse(line),*method,*id,*response,*result;
        if(!request)continue;
        if(!json_object_object_get_ex(request,"method",&method)){json_object_put(request);continue;}
        if(!json_object_object_get_ex(request,"id",&id)){json_object_put(request);continue;}
        response=json_object_new_object();json_object_object_add(response,"jsonrpc",json_object_new_string("2.0"));json_object_object_add(response,"id",json_object_get(id));result=json_object_new_object();
        if(!strcmp(json_object_get_string(method),"initialize")){
            json_object_object_add(result,"protocolVersion",json_object_new_string("2025-11-25"));
            json_object_object_add(result,"capabilities",json_object_new_object());
            json_object_object_add(result,"serverInfo",json_object_new_object());
        }else if(!strcmp(json_object_get_string(method),"tools/list")){
            json_object *tools=json_object_new_array(),*tool=json_object_new_object(),*schema=json_object_new_object();
            json_object_object_add(tool,"name",json_object_new_string("echo"));
            json_object_object_add(tool,"description",json_object_new_string("Echo test input"));
            json_object_object_add(schema,"type",json_object_new_string("object"));
            json_object_object_add(tool,"inputSchema",schema);json_object_array_add(tools,tool);json_object_object_add(result,"tools",tools);
        }else if(!strcmp(json_object_get_string(method),"tools/call")){
            json_object *content=json_object_new_array(),*text=json_object_new_object();
            json_object_object_add(text,"type",json_object_new_string("text"));json_object_object_add(text,"text",json_object_new_string("echo-ok"));
            json_object_array_add(content,text);json_object_object_add(result,"content",content);json_object_object_add(result,"isError",json_object_new_boolean(0));
        }
        json_object_object_add(response,"result",result);
        puts(json_object_to_json_string_ext(response,JSON_C_TO_STRING_PLAIN));fflush(stdout);
        json_object_put(response);json_object_put(request);
    }
    return 0;
}
