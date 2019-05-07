# SimpleGC
This is a minimal garbage collector with code mostly taken from Epsilon GC and https://shipilev.net/jvm/diy-gc/
Its for educational and experimental purpose only.

# To add this garbage collector to JDK12 source code follow these instructions

  - Clone JDK repository
  -- `hg clone --debug --verbose http://hg.openjdk.java.net/jdk/jdk`
    * In case hg clone is very slow. You can download zip bundles and then apply the patch.
  - Checkout this repo at some other location
  -- `git clone git@github.com:unmeshjoshi/simgplegc.git`
  - Apply simplegc.patch to jdk source
  -- `hg import simplegc.patch`
  - Build JDK as following
    * `./configure --with-debug-level=slowdebug --with-native-debug-symbols=internal --with-boot-jdk=<jdk12-binary-home>`
    Note that you need a boot jdk to compile jdk source. Run configure with a boot jdk path on your system.
    * `make all`

You should be able to run open this with eclipse cpp editor and debug placing the breakpoints where you want.

