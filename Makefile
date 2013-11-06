
delivery: delivery.c
	cc -o delivery delivery.c

all: delivery
all: README.html
all: FigIllustration.png

README.html: README.md
	markdown $< > $@

FigIllustration.png: FigIllustration.fig
	fig2dev -L png FigIllustration.fig > FigIllustration.png 

