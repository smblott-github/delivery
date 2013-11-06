
delivery: delivery.c
	cc -o delivery delivery.c

all: delivery
all: README.html

README.html: README.md
	markdown $< > $@

