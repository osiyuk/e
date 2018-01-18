kilo: kilo.c
	$(CC) kilo.c -o $@ -Wall -Wextra -pedantic

kbd: terminal_raw.c
	$(CC) terminal_raw.c -o $@ -Wall -Wextra

test: kilo
	cp kilo.c kilo_backup.c
	./kilo kilo_backup.c

compare: kilo
	bash compare.bash

clean:
	rm -f kbd kilo kilo_backup.c

.PHONY: clean compare test
