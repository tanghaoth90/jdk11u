
package gc.z;

/*
 * @test TestHighUsageLargeHeap
 * @requires vm.gc.Z & !vm.graal.enabled
 * @summary Test ZGC Avoid "Page Cache Flush" by Enabling -XX:+ZBalancePageCache
 * @library /test/lib
 * @run main/othervm/timeout=600 gc.z.TestAvoidPageCacheFlush
 */

import java.util.Random;
import java.util.concurrent.locks.LockSupport;

import jdk.test.lib.Platform;
import jdk.test.lib.process.ProcessTools;

public class TestAvoidPageCacheFlush {

    public static void main(String[] args) throws Exception {
        if (Platform.isSlowDebugBuild()) {
            return; // ignore slow debug build because allocation is too fast for garbage collection
        }

        int heapSizeGB = 4; // 4GB

        ProcessTools.executeTestJvm(new String[]{"-XX:+UnlockExperimentalVMOptions",
                                                 "-XX:+UseZGC",
                                                 "-XX:+UnlockDiagnosticVMOptions",
                                                 "-Xms" + heapSizeGB + "g",
                                                 "-Xmx" + heapSizeGB + "g",
                                                 "-XX:ParallelGCThreads=2",
                                                 "-XX:ConcGCThreads=4",
                                                 "-XX:+ZBalancePageCache", // page cache flush if this line is removed
                                                 "-Xlog:gc,gc+heap",
                                                 AvoidPageCacheFlush.class.getName(),
                                                 Integer.toString(heapSizeGB)})
                                                 .shouldNotContain("Allocation Stall")
                                                 .shouldNotContain("Page Cache Flushed")
                                                 .shouldHaveExitValue(0);
    }

    static class AvoidPageCacheFlush {
        /*
         * small object: fast allocation
         * medium object: slow allocation, periodic deletion
         */
        public static void main(String[] args) throws Exception {
            long heapSizeKB = Long.valueOf(args[0]) << 20;

            SmallContainer smallContainer = new SmallContainer((long)(heapSizeKB * 0.4));     // 40% heap for live small objects
            MediumContainer mediumContainer = new MediumContainer((long)(heapSizeKB * 0.3));  // 30% heap for live medium objects

            int totalSmall = smallContainer.getTotalObjects();
            int totalMedium = mediumContainer.getTotalObjects();

            int addedSmall = 0;
            int addedMedium = 1; // should not be divided by zero

            while (addedMedium < totalMedium * 10) {
                if (totalSmall / totalMedium > addedSmall / addedMedium) { // keep the ratio of allocated small/medium objects
                    smallContainer.createAndSaveObject();
                    addedSmall ++;
                } else {
                    mediumContainer.createAndAppendObject();
                    addedMedium ++;
                }
                if ((addedSmall + addedMedium) % 50 == 0) {
                    LockSupport.parkNanos(500); // make allocation slower
                }
            }
        }
    }

    static class SmallContainer {
        private final int KB_PER_OBJECT = 64; // 64KB per object
        private final Random RANDOM = new Random();

        private byte[][] smallObjectArray;
        private long totalKB;
        private int totalObjects;

        SmallContainer(long totalKB) {
            this.totalKB = totalKB;
            totalObjects = (int)(totalKB / KB_PER_OBJECT);
            smallObjectArray = new byte[totalObjects][];
        }

        int getTotalObjects() {
            return totalObjects;
        }

        // random insertion (with random deletion)
        void createAndSaveObject() {
            smallObjectArray[RANDOM.nextInt(totalObjects)] = new byte[KB_PER_OBJECT << 10];
        }
    }

    static class MediumContainer {
        private final int KB_PER_OBJECT = 512; // 512KB per object

        private byte[][] mediumObjectArray;
        private int mediumObjectArrayCurrentIndex = 0;
        private long totalKB;
        private int totalObjects;

        MediumContainer(long totalKB) {
            this.totalKB = totalKB;
            totalObjects = (int)(totalKB / KB_PER_OBJECT);
            mediumObjectArray = new byte[totalObjects][];
        }

        int getTotalObjects() {
            return totalObjects;
        }

        void createAndAppendObject() {
            if (mediumObjectArrayCurrentIndex == totalObjects) { // periodic deletion
                mediumObjectArray = new byte[totalObjects][]; // also delete all medium objects in the old array
                mediumObjectArrayCurrentIndex = 0;
            } else {
                mediumObjectArray[mediumObjectArrayCurrentIndex] = new byte[KB_PER_OBJECT << 10];
                mediumObjectArrayCurrentIndex ++;
            }
        }
    }
}
