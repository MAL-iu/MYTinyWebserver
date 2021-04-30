#include "headers/mysql_data.h"


 
MyDB::MyDB()     
{
    mysql = mysql_init(NULL);
    if(mysql == NULL)
    {
        printf("Error: %s\n",mysql_error(mysql));
        throw std::exception();
    }           
}
 
MyDB::~MyDB()
{                                                                                                                     
    if(!mysql)
    {
        mysql_close(mysql);
    }
}
 
bool MyDB::InitDB(const char* host,const char* user,const char* pwd,const char* dbname)
{
    /*连接数据库*/
    if(!mysql_real_connect(mysql,host,user,pwd,dbname,0,NULL,0))
    {
        printf("connect fail: %s\n",mysql_error(mysql));
        throw std::exception();
        return false;
    }
    return true;
}
 

char * MyDB::ExeSQL(std::string &sql)
{
    /*执行失败*/
    if(mysql_query(mysql,sql.c_str()))
    {
        printf("query fail: %s\n",mysql_error(mysql));
        throw std::exception();
        return NULL;
    }
 
    else
    {
        
        /*获取结果集*/
        result = mysql_store_result(mysql);
        if(result==NULL)
            return NULL;

        int fieldnum = mysql_num_fields(result);


        if(fieldnum==0)
        {
            return NULL;
        }
        row = mysql_fetch_row(result);
        if(row<=0)
        {
            return NULL;
        }
        return row[0];
        mysql_free_result(result);
    }
    return NULL;
} 

