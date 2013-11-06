Delivery
========

Stream the output of one command to multiple (dynamic) clients.

Build
=====

Just:

  - `make delivery`

Install
=======

Copy the binary wherever is convenient.  For example, one of:

   - `install -vm 0555 delivery ~/local/bin`
   - `sudo install -vm 0555 delivery /usr/local/bin`

Explanation
===========

`delivery` streams the standard output of one command/process (the server) to the
standard input of some number of other processes (the clients) via a Unix
domain socket in `/tmp`.

Clients may come and go.  When the final client dies, the server exits.

An example may help.

I have an ARM-based NAS device with an FM tuner card attached.  I want to
stream MP3 to various devices throughout my house.  Unfortunately, a single
`lame` process requires about 30% of the available CPU cycles.  So I'd like to
have a *single* `lame` process converting the FM audio to MP3 and then multiple
clients reading that process's output.  This is what `delivery` does.

Write a script `encode.sh` which reads the FM audio and encodes it to MP3 on
standard output.  Then run:

  - `delivery -n tuner sh encode.sh`

Nothing happens.  Soon, a client comes along:

   - `delivery -n tuner -c cat`

`delivery` connects the client to the server via a Unix domain socket in
`/tmp`.  When this first client connects, the server calls the command (as
indicated above), and arranges for the command's output to be provide as the
standard input to the client's command (`cat`, here).  The standard output of
the client is the MP3 stream.

Further clients can  connect, and each receives its own copy of the output
stream.  However, there is only ever *one* instance of the `encode.sh` script
run, so only one instance of lame.

Clients may come and go.  When the final client disconnects, the server closes
the `encode.sh` script and exits.  If the server is to run persistently,
consider using `supervise` from D. J. Bernstein's excellent [Daemon Tools
package](http://cr.yp.to/daemontools.html).

Arguments
=========

Server:

   - `delivery [OPTIONS] command [arguments...]`

Client:

   - `delivery [OPTIONS] -c [command [arguments...]]`
   - If `[command [arguments...]]` is omitted, then `cat` is assumed.

Options:

   - `-n BASENAME` -- a name to use as the base name of files in `/tmp`.

If no `-n BASENAME` option is provided, then a basename derived from the name
of the current working directory will be used.

Notes
=====

The server obtains an exclusive lock on a file in `/tmp`.  Therefore, two or
more servers with the same `BASENAME` cannot be run at the same time.  If no
`BASENAME` option is provided, then one is derived from the name of the current
working directory.  In such a case (so only if no `BASENAME` option is
provided), no two or mare servers can be run in the same working directory.

Called as:

   - `delivery [-n BASENAME] -r`

`delivery` sends a `HUP` signal to the relevant server process.  The server
process then closes its data generation process and restarts it.  Clients
remain connected.

`delivery` makes no guarantee as to data alignment.  Newly-connecting clients
simply receive the data stream from the point at which it is when the client
happens to connect.  This tends not to be a problem for multimedia data, for
which frame-boundary markers allow players to recover from incomplete frames.

