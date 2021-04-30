#ifndef _MYSQL_DATA_
#define _MYSQL_DATA_

#include <mysql/mysql.h>
#include <iostream>
#include <stack>
#include <algorithm>      
#include <mysql/mysql.h>
#include <string>


class MyDB
{
public:
    MyDB();
    ~MyDB();
    bool InitDB(const char *host,const char *user,const char *pwd,const char *dbname);
    // bool ExeSQL(char *sql);
    char *ExeSQL(std::string &sql);

private:
    MYSQL *mysql;
    MYSQL_ROW row;
    MYSQL_RES *result;
    MYSQL_FIELD *field;
};

#endif