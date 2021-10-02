# command

is a fork of the [shell] external from Günter Geiger's [ggee](https://github.com/pd-externals/ggee)
library.

It provides a way to execute commands from Pd. Its goal is to
give more fine-grained control over the process it manages. Unlike
the original [shell] external, [command] executes commands directly
and does not wrap them into a shell. This means shell features
cannot be used with [command]. However, they can be used in
shell scripts that can be executed with [command].


## Features

  * `kill` method for sending SIGINT to currently running command.
  * `send` method for sending data to STDIN of currently running command.
  * separate outlet for STDERR of command.
  * report exit code on left-most outlet.
  * use dedicated method `exec` for command execution to avoid conflicts
    between command and method names.
  * search relative to patch instead of relative to Pd when calling
    commands with relative path. This makes projects using custom scripts
    or binaries more portable.
  * `-b` flag for binary output. This can be used to bypass FUDI parsing.
    Useful for cases when characters like leading or trailing spaces
    should be kept.


## Authors

2002 - 2006 Guenter Geiger <geiger@xdv.org>  
2005 - 2010 Hans-Christoph Steiner <hans@eds.org>  
2008 - 2021 IOhannes m zmölnig <zmoelnig@iem.at>  
2019        jyg <jyg@gumo.fr>  
2021        Roman Haefeli <reduzent@gmail.com>  

## License

Tcl/Tk License. Refer to [LICENSE.txt](LICENSE.txt)
