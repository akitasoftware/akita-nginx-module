# akita-ngx-module

Nginx module for mirroring request and response data to the Akita agent.


## Build instructions

Check out Nginx source and this repo.  In the Nginx directory, run

```
./configure --add-dynamic-module=/path/to/akita
make
```

To build with debug mode, SSL support, and AddressSantizer, use:

```
./configure --add-dynamic-module=/path/to/akita --with-http_ssl_module --with-debug --with-cc-opt=-fsanitize=address --with-ld-opt=-fsanitize=address
```

## Example configuration


```
load_module /home/mark/nginx-1.23.2/objs/ngx_http_akita_module.so;

events {}

error_log logs/error.log info;

http {
  server {
     listen 8888;
     server_name example.com;

     akita_agent localhost:5555;

     location / {
        proxy_pass http://127.0.0.1:8000/;
        akita_enable on;
     }
 
     # This is a placeholder that will be replaced by a proper upstream.
     location /akita {
        proxy_pass http://localhost:5555/;
        proxy_http_version 1.1;
        proxy_set_header   "Connection" "";
     }
  }
}
```
