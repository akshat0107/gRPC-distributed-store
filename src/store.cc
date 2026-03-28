#include "threadpool.h"

#include<fstream>
#include<map>
#include<vector>
#include<sstream>
#include <cstdlib>
#include <iostream>
#include<grpcpp/grpcpp.h>
#include "store.grpc.pb.h"
#include "vendor.grpc.pb.h"

#include<string>
#include<thread>
#include<memory>
#include<functional>
#include<cassert>

using grpc::Channel;
using grpc::ServerContext;
using grpc::Status;
using grpc::CompletionQueue;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::Server;

using grpc::ClientContext;
using grpc::ClientAsyncResponseReader;
using grpc::ServerCompletionQueue;

using vendor::Vendor;
using vendor::BidQuery;
using vendor::BidReply;

using store::ProductInfo;
using store::ProductReply;
using store::Store;


class Store_Server 
{
	private:
		std::unique_ptr<grpc::Server> server_;
		std::unique_ptr<grpc::ServerCompletionQueue> cq_;
		Store::AsyncService service_;

		std::map<std::string,int> vendor_next_index;
		
		std::map<std::string,std::vector<std::string>> vendor_map;
		std::string filename;	
		std::mutex index_mutex;

	public:
		~Store_Server()
		{
			server_->Shutdown();
			cq_->Shutdown();
		}
		
		void readFileToString(const std::string& filename)
		{
		char targetletter = '-';
		std::ifstream file(filename,std::ios::binary|std::ios::in);
		if(!file.is_open())
		{
			std::cerr<<"Error: Failed to open file "<<filename<<std::endl;
			return;
		}

		std::string line;
		std::string currentVendor;

		while(std::getline(file,line))
		{
			if(!line.empty() && line.front()!=targetletter){
				currentVendor = line;
				if(currentVendor.back()==' ' or currentVendor.back()=='\r')
				{
					currentVendor.pop_back();
				}
				vendor_map[currentVendor] = std::vector<std::string>{};
			}
			else if (line[0]=='-'){
				std::string port = line.substr(1);
				size_t start = port.find_first_not_of(" \t");
				if (port.back()=='\r')
				{
					port.pop_back();
				}
				if(start!=std::string::npos)
				{
					port = port.substr(start);
				}
				vendor_map[currentVendor].push_back(port);
			}
		}

		file.close();
	}

		Store_Server(std::string file_name):filename(file_name)
		{
			readFileToString(filename);
			for(auto& [vendor,_]:vendor_map)
			{
				vendor_next_index[vendor] = 0;
			}
		}

		void queryAllVendors(const std::string& product_name,ProductReply* final_reply)
		{
			grpc::CompletionQueue cq;

			struct PendingRequest{
				std::string vendor_name;
				vendor::BidReply reply;
				grpc::Status status;
				std::unique_ptr<grpc::ClientContext> context;
				std::unique_ptr<vendor::Vendor::Stub> stub;
			};

			std::vector<std::unique_ptr<grpc::ClientAsyncResponseReader<vendor::BidReply>>> rpcs;

			std::vector<PendingRequest> pending;
			pending.reserve(vendor_map.size());
			rpcs.reserve(vendor_map.size());

			int idx = 0;
			for(auto& [vendor_name,host_addr]: vendor_map)
			{
				int current_idx;
				{
					std::lock_guard<std::mutex> lock(index_mutex);
					current_idx = vendor_next_index[vendor_name];
					vendor_next_index[vendor_name] = (current_idx+1)%host_addr.size();
				}
				
				std::string host_address = host_addr[current_idx];
				
				auto channel = grpc::CreateChannel(host_address,grpc::InsecureChannelCredentials());

				pending.emplace_back();
				pending.back().vendor_name = vendor_name;
				pending.back().stub = vendor::Vendor::NewStub(channel);
				pending.back().context = std::make_unique<grpc::ClientContext>();

				vendor::BidQuery query;
				query.set_product_name(product_name);

				auto rpc = pending.back().stub->PrepareAsyncgetProductBid(pending.back().context.get(),query,&cq);

				rpc->StartCall();
				rpc->Finish(&pending.back().reply,&pending.back().status,(void*)(intptr_t)idx);

				rpcs.push_back(std::move(rpc));

			idx++;
			}

		int received = 0;
		while(received<pending.size())
		{
			void* tag;
			bool ok;
			cq.Next(&tag,&ok);

			int completed_idx = (intptr_t)tag;
			auto& completed = pending[completed_idx];

			if(completed.status.ok())
			{
				auto* product = final_reply->add_products();
				product->set_price(completed.reply.price());
				product->set_vendor_id(completed.reply.vendor_id());
				
			}
			received++;
		}
		cq.Shutdown();

		}	
		class CallData_{
			private:
				Store::AsyncService* service_;
				grpc::ServerCompletionQueue* cq_;
				grpc::ServerContext ctx_;
				store::ProductQuery request_;	
				store::ProductReply reply_;
				grpc::ServerAsyncResponseWriter<ProductReply> responder_;
				enum Status { CREATE, PROCESS, FINISH};
				Status status_;
				Store_Server* store_;

		public:
		CallData_(Store::AsyncService* service,grpc::ServerCompletionQueue *cq, Store_Server* store):service_(service),cq_(cq),responder_(&ctx_),status_(CREATE),store_(store){
					proceed();}

		void proceed(){
			if(status_==CREATE){
				status_= PROCESS;

				service_->RequestgetProducts(&ctx_,&request_,&responder_,cq_,cq_,this);
			}
			else if(status_==PROCESS)
			{
				new CallData_(service_,cq_,store_);
				store_->queryAllVendors(request_.product_name(),&reply_);

				status_= FINISH;
				responder_.Finish(reply_,grpc::Status::OK,this);	
			}

			else if (status_==FINISH)
			{
				delete this;
			}
		}
	};

	void runStore(std::string addr, int num_threads)
		{
			ServerBuilder builder;
			builder.AddListeningPort(addr,grpc::InsecureServerCredentials());

			builder.RegisterService(&service_);

			cq_ = builder.AddCompletionQueue();

			server_ = builder.BuildAndStart();

			threadpool pool(num_threads);
			new CallData_(&service_,cq_.get(),this);
			
			void* tag;
			bool ok;
			while(true)
			{
				cq_->Next(&tag,&ok);
				CallData_* cd = static_cast<CallData_*>(tag);
				pool.enqueue([cd](){cd->proceed();});
			}

		}

};

std::string store_address;
std::string vendor_file;
int num_threads_global;

int run_store(){
	Store_Server store(vendor_file);
	store.runStore(store_address,num_threads_global);
	return 0;
}


int main(int argc, char** argv) {
	if(argc!=4)
	{
		std::cerr<<"Usage: ./store <vendor_file> <address> <threads>";
		return 1;
	}

	vendor_file = argv[1];
	store_address = argv[2];
	num_threads_global = std::stoi(argv[3]);

	run_store();

	return EXIT_SUCCESS;

}
