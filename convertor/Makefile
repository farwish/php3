convertor:	language-scanner.o language-parser.tab.o token_cache.o main.o alloc.o
	gcc -o convertor language-scanner.o language-parser.tab.o token_cache.o main.o alloc.o

language-parser.tab.c language-parser.tab.h:	language-parser.y token_cache.h
	bison -p php -v -d language-parser.y

language-scanner.c:		language-scanner.lex token_cache.h
	flex -i -Pphp -olanguage-scanner.c language-scanner.lex


language-parser.tab.o:	language-parser.tab.c token_cache.h
	gcc -c language-parser.tab.c -o language-parser.tab.o

language-scanner.o:		language-scanner.c token_cache.h language-parser.tab.h
	gcc -c language-scanner.c -o language-scanner.o

token_cache.o:	token_cache.c token_cache.h
	gcc -c token_cache.c -o token_cache.o

main.o:		main.c token_cache.h
	gcc -c main.c -o main.o

alloc.o:	alloc.c token_cache.h
	gcc -c alloc.c -o alloc.o

clean:
	rm -f *.o *.tab.c *.output language-scanner.c convertor

