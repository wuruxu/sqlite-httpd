all:sqlitehttpd.c
	gcc -o sqlitehttpd sqlitehttpd.c sqlite-http-req.c sqlite-json-info.c -I./ -Iexternal-libs/include -Lexternal-libs/lib/ -lmicrohttpd -lsqlite3 -levent -ljson-c -lz
