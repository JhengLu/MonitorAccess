# Monitor Access Latency
<p>This code can be used for calculating the access latency</p>
<p>Memchached: </p>
<img width="451" alt="image" src="https://github.com/JhengLu/MonitorAccess/assets/77672985/144f0686-b181-40ec-a394-ae8df2916ad4">
<p>Redis: </p>
<img width="429" alt="image" src="https://github.com/JhengLu/MonitorAccess/assets/77672985/b143bc3b-45b0-487f-9874-113ebc14ed2d">

How to run?

```
cd /MonitorAccess
mkdir build
cd build
sudo apt install cmake
cmake ..
make
```
How to measure different applications?
```
# change the name of application here
monitor.measure_process_latency("redis-server");
```
How to measure different processes?

```
# change the pid here
monitor.measure_process_latency(pid);
```
