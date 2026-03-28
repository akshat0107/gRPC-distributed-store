DISTRIBUTED STORE WITH gRPC AND THREADPOOL

OVERVIEW

This project implements a distributed store service using gRPC. The store sits between clients and a set of registered vendors, collecting price bids from all vendors for a requested product and returning them to the client. Requests are handled concurrently using a custom threadpool, and vendor communication is done asynchronously to maximize throughput.


THREADPOOL

The threadpool is implemented in threadpool.h using POSIX threads (pthread) with a shared job queue protected by a mutex and condition variable.

PRIVATE MEMBERS:
  - std::vector<pthread_t> threads  : stores the POSIX thread handles
  - std::queue<std::function<void()>> q : FIFO job queue
  - std::mutex m_mutex              : protects the queue and shutdown flag
  - std::condition_variable m_cond  : workers sleep on this until jobs arrive
  - bool shutdown                   : signals workers to stop

CONSTRUCTOR:
  threadpool(unsigned int num_threads) pre-sizes the threads vector and spawns N POSIX threads via pthread_create. Each thread runs entry_point(), a static bridge function required because pthread_create cannot take a member function directly. entry_point() casts the void* arg back to a threadpool* and calls worker_loop().

WORKER LOOP:
  Each thread runs worker_loop() in an infinite while(1) loop. It acquires m_mutex and sleeps on m_cond.wait() until either a job is available or 
  shutdown is true. On wakeup, if shutdown is true and the queue is empty the thread exits. Otherwise it dequeues the front job, releases the lock before
  executing, then calls the job. Releasing the lock before execution is critical holding it during execution would serialize all threads and defeat the
  purpose of the pool.

ENQUEUE:
  Jobs are submitted via enqueue(std::function<void()>), which locks m_mutex, pushes the job to the back of the queue, and calls m_cond.notify_one() to
  wake exactly one sleeping worker.

SHUTDOWN:
  The destructor locks m_mutex, sets shutdown=true, releases the lock, then calls m_cond.notify_all() to wake all sleeping workers. Each worker sees
  shutdown=true and q.empty(), exits its loop, and the destructor joins all threads via pthread_join. Jobs already in the queue when shutdown is set are
  still processed before threads exit.



COMMUNICATION PIPELINES

The store has two distinct async gRPC pipelines- one facing the client, and one facing the vendors.

CLIENT-FACING PIPELINE (Client -> Store)

The store registers a Store::AsyncService and uses a ServerCompletionQueue to receive incoming client requests asynchronously. A CallData_ object represents one in-flight client request and moves through three states: CREATE, PROCESS and FINISH. The main thread runs a polling loop that calls cq_->Next() to block until an event is ready. When one arrives, the associated CallData_ is dispatched to the threadpool:

    while(true) {
        cq_->Next(&tag, &ok);
        CallData_* cd = static_cast<CallData_*>(tag);
        pool.enqueue([cd]() { cd->proceed(); });
    }

This keeps the polling loop non-blocking. It hands off work immediately and goes back to waiting. The actual processing happens on worker threads.

Inside proceed(), the state machine works like this:

CREATE: The CallData_ registers itself with the service via RequestgetProducts(), telling gRPC that it is ready to receive a request. State advances to PROCESS.

PROCESS: A new CallData_ is created immediately to accept the next incoming request while this one is being handled. Then queryAllVendors() is called to collect bids from all vendors. Once results are ready, status_ is set to FINISH and responder_.Finish() sends the reply back to the client.

It is important that status_ is updated before calling Finish(). Finish() puts this CallData_ back on the completion queue and another thread could pick it up and call proceed() again before the current thread gets a chance to update the state, causing a double-Finish error.

FINISH:  The CallData_ deletes itself. Its job is done.



VENDOR-FACING PIPELINE (Store -> Vendors)

queryAllVendors() is called on a worker thread and handles all vendor communication for a single client request. It contacts every vendor concurrently using async gRPC client calls, waits for all responses, and populates the reply before returning.

Each call to queryAllVendors() creates its own local CompletionQueue. With a local queue, each call is fully self-contained.

Round-robin load balancing determines which backend to use for each vendor. A per-vendor index is maintained globally across the store process and incremented on each request. Access to this index is protected by a mutex so that concurrent calls don't interfere with each other's selection.

For each vendor, a fresh gRPC channel and stub are created using the selected backend address. The RPC is initiated with PrepareAsyncgetProductBid() followed by an explicit StartCall() and Finish(). The PrepareAsync variant is used here because it does not call StartCall() internally, the Async variant does, so calling StartCall() on top of it would trigger it twice and cause a crash.

The async response reader returned by PrepareAsyncgetProductBid() is stored in a vector for the duration of the call. If it were not stored, the unique_ptr would be destroyed at the end of each loop iteration while gRPC still holds pointers to the reply and status buffers, resulting in memory corruption.

Similarly, the pending request vector is reserved upfront before the loop to prevent reallocation. Reallocation would move the vector's contents in memory, invalidating any pointers that were already handed to gRPC.

Once all RPCs are sent, the function drains the queue until all responses are received. 

 while(received < pending.size()) {
        cq.Next(&tag, &ok);
        int idx = (intptr_t)tag;
        if(pending[idx].status.ok()) {
            auto* product = final_reply->add_products();
            product->set_price(pending[idx].reply.price());
            product->set_vendor_id(pending[idx].reply.vendor_id());
        }
        received++;
    }
    cq.Shutdown();

Responses with a failed status are silently skipped and the store
returns whatever bids it successfully collected, handling unavailable vendor backends gracefully without failing the entire request.

