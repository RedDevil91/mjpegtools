This project was forked from [mjpegtools](https://github.com/silicontrip/mjpegtools)
to create an image stream using jpeg2yuv and mpeg2enc.

##How to compile the code

* Build the attached Dockerfile
    ```docker build -t mjpeg-dev```
* Run the docker image in interactive mode and mount this folder
    ```docker run -it -v <path_to_this_repo>:/mjpegtools```
* Change the directory and configure and make the project
    ```bash
    cd mjpegtools
    ./configure
    make
    ```
##Test the application
To create a sample mpeg video from a test image stream, 
you can use the js scripts from this [repo](https://github.com/balint-s/ffmpeg-stream-test)
    
```bash
node sender.js | lavtools/jpeg2yuv -f 25 -I p | mpeg2enc/mpeg2enc -f 0 -o test > video.mpeg
```

_Note: the `-o test` parameter is necessary but doesn't have any effect :)_