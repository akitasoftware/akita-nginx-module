# akita-nginx-module

This module mirrors request and response data from NGINX to the Akita agent. 
With this capability, you can see and monitor traffic that would not otherwise be visible
to the Akita agent because it is encrypted or sent over a Unix domain socket,
and you are using NGINX as a reverse proxy.

## Getting started

1. Create an account on the [Akita App](http://app.akita.software/).

2. In the Akita App, go through the onboarding steps to create a project
and an API Key.  Record this API key for the next steps.

3. On the machine where you run NGINX, download the Akita agent by running our auto-install script:
```
bash -c "$(curl -L https://releases.akita.software/scripts/install_akita.sh)"
```

4. Run `akita login` and enter your API key.

5. Run `akita nginx download`, which will find the version of NGINX running on your system, verify that it is supported, and download a precompiled dynamic module. 
If no module is available for your version of NGINX, please [contact Akita](support@akitasoftware.com) or see [Building the module from source](#building-the-module-from-source).

6. Run `akita nginx collect --project {your project}` which will start listening for traffic from NGINX. By default this runs on port 50080, though you can specify a different one.

7. Edit your NGINX configuration file to load the Akita module and enable it in one of the server or location blocks.

   * Add `load_module ngx_http_akita_module.so` to the top of your configuration file.
   * Add `akita_agent localhost:50080;` to the `http {}` block that handles the traffic you wish to monitor.  If you picked a different port number, use that instead of 50080
   * Add `akita_enable on;` to the `http {}`, `server {}` or `location {}` block for the the traffic you wish to monitor. You can enable or disable monitoring on each location.
      
8. Reload the NGINX configuration with `nginx -s reload`

9. Check whether the Akita agent indicates whether traffic is successfully being captured and uploaded to the Akita cloud.  If not, please contact support@akitasoftware.com for assistance, including any output from the Akita agent, your NGINX configuration, and your NGINX log file.

### Prebuilt modules

Prebuilt modules are currently available for NGINX Plus release 28 (1.23.2) and R27 (1.21.6) on the Linux x86_64 platform. 
We also have a module for the open source NGINX version 1.22.0.  You can download this module from the [latest release](https://github.com/akitasoftware/akita-nginx-module/releases/latest) on GitHub.

Please [contact Akita](mailto:support@akitasoftware.com) to request support for additional versions of NGINX!

After downloading the module, install it in `/etc/nginx/modules` and create a symbolic link with the non-version-specific module name:

```
$ sudo ln -s ngx_http_akita_module_1.23.2.so /etc/nginx/modules/ngx_ahttp_akita_module.so
```

### Building the module from source

To use the Akita module with other versions of NGINX, you must compile it from source.  First determine the version of NGINX you are running.  (For NGINX Plus, this will match a corresponding open-source release of NGINX.)

```
$ nginx -v
nginx version: nginx/1.23.2
```

Download and unpack the open source NGINX package for this version:

```
$ wget https://nginx.org/download/nginx-1.23.2.tar.gz
$ tar -xzvf nginx-1.23.2.tar.gz
```

In a separate directory, download the latest release of the Akita NGINX module:

```
$ git clone --branch release  https://github.com/akitasoftware/akita-nginx-module.git
```

In the NGINX source directory, build the module:

```
$ cd nginx-1.23.2/
$ ./configure --with-compat --add-dynamic-module=../akita-nginx-module
$ make modules
```

Copy the module from the `objs` directory to `/etc/nginx/modules`:

```
$ sudo cp objs/ngx_http_akita_module.so /etc/nginx/modules
```

Then add the `load_module` directive as described in step 7 of the [Getting started](#getting-started) instructions, or see the [Module configuration](#module-configuration) section below.

### Running Akita

The Akita module will attempt to send traffic to the Akita agent at the hostname and port that you have configured (or, by default, `localhost:50080`.)  You should normally run the Akita agent
and NGINX on the same machine; this will reduce overhead and minimize exposure of your traffic.

The Akita agent needs an API token in order to communicate with the Akita cloud.  You can either enter it via `akita login` (which will store it in a configuration file) or use the 
`AKITA_API_KEY_ID` and `AKITA_API_KEY_SECRET` environment variables.

Once you have an API key, you can run 

```
akita nginx collect --project {your project name}
```

to start listening for traffic from NGINX. The Akita agent will log when it starts successfully retrieving traffic from the module, and any errors connecting with the Akita Cloud.

You may optionally change which port `akita` listens on with the `--port` command line flag. Run `akita nginx collect --help` for other available options.

If the Akita agent is not running, the module will back off and pause mirroring traffic.  We recommend starting `akita nginx collect` before `nginx` to avoid an interruption in monitoring.

### Example configuration file

This configuration file shows how to use Akita to monitor 
all the traffic being sent to your web service, when using
NGINX as a reverse proxy.

```
load_module modules/ngx_http_akita_module.so;

http {
  server {
     # Set up a TLS listener
     listen 443 ssl;
     server_name example.com;
     ssl_certificate www.example.com.crt;
     ssl_certificate www.example.com.key;
     ssl_protocols TLSv1.2 TLSv1.3;

     # Send to the Akita agent over HTTP
     akita_agent localhost:50080;

     # Enable Akita on the location that is configured
     # to forward traffic to your service.
     location / {
        proxy_pass http://myservice/;
        akita_enable on;
     }
  }
  
  upstream server1 {
     server unix:/var/run/myservice.sock;
  }
}
```


## Module configuration

The Akita module must be dynamically loaded by the NGINX configuration file.  Add a line

```
load_module modules/ngx_http_akita_module.so;
```

to the beginning of your `nginx.conf` to install the module.  Then add 

```
akita_enable on;
```

to a `http`, `server`, or `location` section in your configuration, in order 
to start monitoring the corresponding HTTP traffic.  The Akita module's configuration
directives are described in more detail in the next section.

### Configuration directives 

#### `akita_agent <host:port>;`

The host and port should match the location where the Akita agent is accepting traffic for analysis.  The default is `localhost:50800`; the directive is optional if that default is OK.

This directive can be placed at the top level, inside a server block, or inside a location block.

The traffic between the Nginx server and the Akita agent is unencrypted. It is best to run both on the same host, but it is possible to mirror the traffic to an Akita agent running on a separate host, 
as long as the host name is specified in the akita_agent directive.

#### `akita_enable [on|off];`

This directive enables or disables collection of traffic within the matching scope.  The default is `off`.

You may enable traffic for all HTTP traffic, for a particular server, or for a particular location.  You can also selectively
disable traffic to a location even if mirroring is enabled in an enclosing scope.

#### `akita_max_body_size <size>;`

Limit the size of a body captured by Akita to the specified amount; any data after that amount is truncated.  Default is 1MiB.

Note that the standard `client_max_body_size` limit could be lower (it is also 1 MiB by default.)

## Limitations / Known Issues

* The Akita module cannot track HEAD requests.

* Some of the NGINX integration tests related to If-Modified and If-Match fail when the Akita module is enabled.

## Developer build instructions

Check out NGINX source and this repository.  In the NGINX directory, run

```
./configure --add-dynamic-module=/path/to/akita
make
```

To build with debug mode, SSL support, and AddressSantizer, use:

```
./configure --add-dynamic-module=/path/to/akita --with-http_ssl_module --with-debug --with-cc-opt=-fsanitize=address --with-ld-opt=-fsanitize=address
```

To enable debug logging and run NGINX in the foreground, add the following to the configuration file:

```
worker_processes 1;
daemon off;

error_log logs/error.log debug;
```

The Akita CLI includes a development mode which only dumps the traffic it receives.  Run `akita nginx capture --dev` to enable this mode.




