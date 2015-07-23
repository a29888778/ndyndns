ndyndns is a multi-service dynamic DNS update client written in C. It avoids unnecessary updates, reports errors, logs to syslog, and generally ought to be compliant with everything required for a proper DDNS client. It is written to be secure and supports running as an unprivileged process in a chroot jail.  SSL-encrypted transfers are also supported.

It currently supports Hurricane Electric (DDNS and IPv6 tunnel updates), Dyndns.com, and Namecheap DDNS services.

ndyndns is dependent on cURL and currently works on Linux and the BSDs, but is easily portable to other POSIX platforms.