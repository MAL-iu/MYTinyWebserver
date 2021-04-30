app : *.cpp
	g++ -o app *.cpp -pthread -lmysqlclient -L/usr/include/mysql
