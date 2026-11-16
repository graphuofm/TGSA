// E35: what a "query" costs on Kafka. Consume the whole topic (one consumer thread per
// partition, manual assignment, from the beginning to the end offsets captured at start)
// and filter client side:
//   q1        lo <= ts <= hi                       count, chk = sum(ts ^ (dst<<32) ^ src)
//   q2        src == v_i and lo_i <= ts <= hi_i    over all queries in the file (or one with --qindex)
//                                                  count, chk = sum(ts ^ dst), once per matching query
// Prints one JSON line. Checksums are u64 sums printed unsigned, as in docs/E33_SPEC.md.
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.apache.kafka.clients.consumer.*;
import org.apache.kafka.common.TopicPartition;
import org.apache.kafka.common.serialization.ByteArrayDeserializer;

public class E35Consumer {
    public static void main(String[] a) throws Exception {
        Map<String,String> arg = new HashMap<>();
        for (int i = 0; i + 1 < a.length; i += 2) arg.put(a[i].replaceFirst("^--", ""), a[i+1]);
        final String bootstrap = arg.getOrDefault("bootstrap", "127.0.0.1:9092");
        final String topic = arg.get("topic");
        final String mode = arg.getOrDefault("mode", "q1");
        final long q1lo = Long.parseLong(arg.getOrDefault("lo", "0"));
        final long q1hi = Long.parseLong(arg.getOrDefault("hi", "0"));
        final int qindex = Integer.parseInt(arg.getOrDefault("qindex", "-1"));

        // queries: N records of three little-endian u64 {v, lo, hi}
        final HashMap<Integer, ArrayList<long[]>> qmap = new HashMap<>();
        int nq = 0;
        if (mode.equals("q2")) {
            byte[] raw = Files.readAllBytes(Paths.get(arg.get("queries")));
            ByteBuffer bb = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            int n = raw.length / 24;
            for (int i = 0; i < n; i++) {
                long v = bb.getLong(), lo = bb.getLong(), hi = bb.getLong();
                if (qindex >= 0 && i != qindex) continue;
                qmap.computeIfAbsent((int) v, k -> new ArrayList<>()).add(new long[]{lo, hi});
                nq++;
            }
        }

        Properties p = new Properties();
        p.put(ConsumerConfig.BOOTSTRAP_SERVERS_CONFIG, bootstrap);
        p.put(ConsumerConfig.ENABLE_AUTO_COMMIT_CONFIG, "false");
        p.put(ConsumerConfig.AUTO_OFFSET_RESET_CONFIG, "earliest");
        p.put(ConsumerConfig.MAX_POLL_RECORDS_CONFIG, "50000");
        p.put(ConsumerConfig.KEY_DESERIALIZER_CLASS_CONFIG, ByteArrayDeserializer.class.getName());
        p.put(ConsumerConfig.VALUE_DESERIALIZER_CLASS_CONFIG, ByteArrayDeserializer.class.getName());

        List<TopicPartition> tps = new ArrayList<>();
        Map<TopicPartition, Long> ends;
        try (KafkaConsumer<byte[],byte[]> probe = new KafkaConsumer<>(p)) {
            for (var pi : probe.partitionsFor(topic)) tps.add(new TopicPartition(topic, pi.partition()));
            tps.sort(Comparator.comparingInt(TopicPartition::partition));
            ends = probe.endOffsets(tps);
        }
        final int threads = tps.size();
        final CyclicBarrier start = new CyclicBarrier(threads + 1);
        final CountDownLatch done = new CountDownLatch(threads);
        final AtomicLong scanned = new AtomicLong(), count = new AtomicLong(), chk = new AtomicLong();
        final long[] tEnd = new long[threads];
        final Map<TopicPartition, Long> endsF = ends;
        for (int t = 0; t < threads; t++) {
            final int tid = t;
            final TopicPartition tp = tps.get(t);
            new Thread(() -> {
                Properties pp = new Properties(); pp.putAll(p);
                pp.put(ConsumerConfig.CLIENT_ID_CONFIG, "e35-consumer-" + tid);
                try (KafkaConsumer<byte[],byte[]> c = new KafkaConsumer<>(pp)) {
                    c.assign(List.of(tp));
                    c.seekToBeginning(List.of(tp));
                    final long end = endsF.get(tp);
                    start.await();
                    long sc = 0, cnt = 0, ck = 0;
                    while (c.position(tp) < end) {
                        ConsumerRecords<byte[],byte[]> recs = c.poll(Duration.ofSeconds(2));
                        for (ConsumerRecord<byte[],byte[]> r : recs) {
                            ByteBuffer bb = ByteBuffer.wrap(r.value()).order(ByteOrder.LITTLE_ENDIAN);
                            long src = Integer.toUnsignedLong(bb.getInt());
                            long dst = Integer.toUnsignedLong(bb.getInt());
                            long ts = bb.getLong();
                            sc++;
                            if (mode.equals("q1")) {
                                if (ts >= q1lo && ts <= q1hi) { cnt++; ck += ts ^ (dst << 32) ^ src; }
                            } else {
                                ArrayList<long[]> qs = qmap.get((int) src);
                                if (qs != null)
                                    for (long[] q : qs) if (ts >= q[0] && ts <= q[1]) { cnt++; ck += ts ^ dst; }
                            }
                        }
                    }
                    tEnd[tid] = System.nanoTime();
                    scanned.addAndGet(sc); count.addAndGet(cnt); chk.addAndGet(ck);
                } catch (Exception e) { e.printStackTrace(); }
                finally { done.countDown(); }
            }, "e35-cons-" + tid).start();
        }
        start.await();
        final long t0 = System.nanoTime();
        done.await();
        long t1 = t0; for (long te : tEnd) t1 = Math.max(t1, te);
        double sec = (t1 - t0) / 1e9;
        System.out.printf(Locale.ROOT,
            "{\"mode\":\"%s\",\"queries\":%d,\"partitions\":%d,\"scanned\":%d,\"count\":%d,\"checksum\":\"%s\",\"seconds\":%.6f,\"scan_rate\":%.1f}%n",
            mode, mode.equals("q1") ? 1 : nq, threads, scanned.get(), count.get(),
            Long.toUnsignedString(chk.get()), sec, scanned.get() / sec);
    }
}
