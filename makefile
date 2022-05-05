
encrypt : encrypt-driver.c encrypt-module.c 
	gcc encrypt-driver.c -g encrypt-module.c -lpthread -o  encrypt

clean :
	rm encrypt
