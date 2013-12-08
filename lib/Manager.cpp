/*
 * Copyright 2013 Dominic Spill
 * Copyright 2013 Adam Stasiak
 *
 * This file is part of USBProxy.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 * Manager.cpp
 *
 * Created on: Nov 12, 2013
 */
#include "Manager.h"
#include "pthread.h"
#include "TRACE.h"

#include "Device.h"
#include "DeviceQualifier.h"
#include "Endpoint.h"
#include "Packet.h"

#include "DeviceProxy.h"
#include "HostProxy.h"
#include "PacketFilter.h"
#include "Relayer.h"
#include "Injector.h"

Manager::Manager(DeviceProxy* _deviceProxy,HostProxy* _hostProxy) {
	status=USBM_IDLE;
	deviceProxy=_deviceProxy;
	hostProxy=_hostProxy;
	device=NULL;
	filters=NULL;
	filterCount=0;
	injectors=NULL;
	injectorCount=0;
	injectorThreads=NULL;
	out_queue_ep0=NULL;
	//out_queue_ep0=NULL;
	int i;
	for(i=0;i<16;i++) {
		in_endpoints[i]=NULL;
		in_relayers[i]=NULL;
		in_relayerThreads[i]=0;
		in_queue[i]=NULL;
		out_endpoints[i]=NULL;
		out_relayers[i]=NULL;
		out_relayerThreads[i]=0;
		out_queue[i]=NULL;
	}
}

Manager::~Manager() {
	if (device) {
		delete(device);
		device=NULL;
	}
	if (filters) {
		free(filters);
		filters=NULL;
	}
	int i;
	for (i=0;i<16;i++) {
		if (in_relayerThreads[i]) {
			pthread_cancel(in_relayerThreads[i]);
			in_relayerThreads[i]=0;
		}
		if (out_relayerThreads[i]) {
			pthread_cancel(out_relayerThreads[i]);
			out_relayerThreads[i]=0;
		}
		if (in_relayers[i]) {
			delete(in_relayers[i]);
			in_relayers[i]=NULL;
		}
		if (out_relayers[i]) {
			delete(out_relayers[i]);
			out_relayers[i]=NULL;
		}
		Packet* p;
		if (in_queue[i]) {
			while(in_queue[i]->pop(p)) {delete(p);/* not needed p=NULL; */}
			delete(in_queue[i]);
			in_queue[i]=NULL;
		}
		if (out_queue[i]) {
			while(out_queue[i]->pop(p)) {delete(p);/* not needed p=NULL; */}
			delete(out_queue[i]);
			out_queue[i]=NULL;
		}
	}
	if (injectorThreads) {
		for (i=0;i<injectorCount;i++) {
			if (injectorThreads[i]) {
				pthread_cancel(injectorThreads[i]);
				injectorThreads[i]=0;
			}
		}
		free(injectorThreads);
		injectorThreads=NULL;
	}
	if (injectors) {
		free(injectors);
		injectors=NULL;
	}

}

void Manager::inject_packet(Packet *packet) {
	if (status!=USBM_RELAYING) {fprintf(stderr,"Can't inject packets unless manager is relaying.\n");}
	__u8 epAddress=packet->bEndpoint;
	if (epAddress&0x80) { //device->host
		in_queue[epAddress&0x0f]->push(packet);
	} else { //host->device
		out_queue[epAddress&0x0f]->push(packet);
	}
}

void Manager::inject_setup_in(usb_ctrlrequest request,__u8** data,__u16 *transferred, bool filter) {
	if (status!=USBM_RELAYING) {fprintf(stderr,"Can't inject packets unless manager is relaying.\n");}
	SetupPacket* p=new SetupPacket(request,NULL,filter);
	out_queue_ep0->push(p);
	//TODO handle returned data...somehow..can use 2nd queue for replies, but would need to poll it or something
}

void Manager::inject_setup_out(usb_ctrlrequest request,__u8* data,bool filter) {
	if (status!=USBM_RELAYING) {fprintf(stderr,"Can't inject packets unless manager is relaying.\n");}
	SetupPacket* p=new SetupPacket(request,data,filter);
	out_queue_ep0->push(p);
}


void Manager::add_injector(Injector* _injector){
	if (status!=USBM_IDLE) {fprintf(stderr,"Can't add injectors unless manager is idle.\n");}
	if (injectors) {
		injectors=(Injector**)realloc(injectors,++injectorCount*sizeof(Injector*));
	} else {
		injectorCount=1;
		injectors=(Injector**)malloc(sizeof(Injector*));
	}
	injectors[injectorCount-1]=_injector;
}

void Manager::remove_injector(__u8 index,bool freeMemory){
	if (status!=USBM_IDLE) {fprintf(stderr,"Can't remove injectors unless manager is idle.\n");}
	if (!injectors || index>=injectorCount) {fprintf(stderr,"Injector index out of bounds.\n");}
	if (freeMemory && injectors[index]) {delete(injectors[index]);/* not needed injectors[index]=NULL;*/}
	if (injectorCount==1) {
		injectorCount=0;
		free(injectors);
		injectors=NULL;
	} else {
		int i;
		for(i=index+1;i<injectorCount;i++) {
			injectors[i-1]=injectors[i];
		}
		injectors=(Injector**)realloc(injectors,--injectorCount*sizeof(Injector*));
	}
}

Injector* Manager::get_injector(__u8 index){
	if (!injectors || index>=injectorCount) {return NULL;}
	return injectors[index];
}

__u8 Manager::get_injector_count(){
	return injectorCount;
}

void Manager::add_filter(PacketFilter* _filter){
	if (status!=USBM_IDLE) {fprintf(stderr,"Can't add filters unless manager is idle.\n");}
	if (filters) {
		filters=(PacketFilter**)realloc(filters,++filterCount*sizeof(PacketFilter*));
	} else {
		filterCount=1;
		filters=(PacketFilter**)malloc(sizeof(PacketFilter*));
	}
	filters[filterCount-1]=_filter;
}

void Manager::remove_filter(__u8 index,bool freeMemory){
	if (status!=USBM_IDLE) {fprintf(stderr,"Can't remove filters unless manager is idle.\n");}
	if (!filters || index>=filterCount) {fprintf(stderr,"Filter index out of bounds.\n");}
	if (freeMemory && filters[index]) {delete(filters[index]);/* not needed filters[index]=NULL;*/}
	if (filterCount==1) {
		filterCount=0;
		free(filters);
		filters=NULL;
	} else {
		int i;
		for(i=index+1;i<filterCount;i++) {
			filters[i-1]=filters[i];
		}
		filters=(PacketFilter**)realloc(filters,--filterCount*sizeof(PacketFilter*));
	}
}

PacketFilter* Manager::get_filter(__u8 index){
	if (!filters || index>=filterCount) {return NULL;}
	return filters[index];
}

__u8 Manager::get_filter_count(){
	return filterCount;
}


void Manager::start_control_relaying(){
	//TODO this should exit immediately if already started, and wait (somehow) is stopping or setting up
	status=USBM_SETUP;

	//connect device proxy
	if (deviceProxy->connect()!=0) {fprintf(stderr,"Unable to connect to device proxy.\n");status=USBM_IDLE;return;}

	//populate device model
	device=new Device(deviceProxy);
	device->print(0);

	//create EP0 endpoint object
	usb_endpoint_descriptor desc_ep0;
	desc_ep0.bLength=7;
	desc_ep0.bDescriptorType=USB_DT_ENDPOINT;
	desc_ep0.bEndpointAddress=0;
	desc_ep0.bmAttributes=0;
	desc_ep0.wMaxPacketSize=device->get_descriptor()->bMaxPacketSize0;
	desc_ep0.bInterval=0;
	out_endpoints[0]=new Endpoint((Interface*)NULL,&desc_ep0);

	//set up queues,relayers,and filters
	out_queue_ep0=new boost::lockfree::queue<SetupPacket*>(16);
	out_relayers[0]=new Relayer(this,out_endpoints[0],deviceProxy,hostProxy,out_queue_ep0);


	//apply filters to relayers
	int i;
	for(i=0;i<filterCount;i++) {
		if (filters[i]->test_device(device)) {
			if (out_endpoints[0] && filters[i]->test_endpoint(out_endpoints[0]) ) {
				out_relayers[0]->add_filter(filters[i]);
			}
		}
	}

	if (hostProxy->connect(device)!=0) {
		stop_relaying();
		return;
	}

	if (injectorCount) {
		injectorThreads=(pthread_t *)calloc(injectorCount,sizeof(pthread_t));
		for(i=0;i<injectorCount;i++) {
			pthread_create(&injectorThreads[i],NULL,&Injector::listen_helper,injectors[i]);
		}
	}

	if (out_relayers[0]) {
		pthread_create(&out_relayerThreads[0],NULL,&Relayer::relay_helper,out_relayers[0]);
	}

	status=USBM_RELAYING;
}

void Manager::start_data_relaying() {
	//enumerate endpoints
	Configuration* cfg;
	cfg=device->get_active_configuration();
	int ifc_idx;
	int ifc_cnt=cfg->get_descriptor()->bNumInterfaces;
	for (ifc_idx=0;ifc_idx<ifc_cnt;ifc_idx++) {
		Interface* ifc=cfg->get_interface(ifc_idx);
		int ep_idx;
		int ep_cnt=ifc->get_endpoint_count();
		for(ep_idx=0;ep_idx<ep_cnt;ep_idx++) {
			Endpoint* ep=ifc->get_endpoint_by_idx(ep_idx);
			const usb_endpoint_descriptor* epd=ep->get_descriptor();
			if (epd->bEndpointAddress & 0x80) { //IN EP
				in_endpoints[epd->bEndpointAddress&0x0f]=ep;
			} else { //OUT EP
				out_endpoints[epd->bEndpointAddress&0x0f]=ep;
			}
		}
	}

	//set up queues,relayers,and filters
	int i,j;
	for (i=1;i<16;i++) {
		if (in_endpoints[i]) {
			//Relayer(Endpoint* _endpoint,DeviceProxy* _device,HostProxy* _host,boost::lockfree::queue<SetupPacket*>* _queue);
			in_queue[i]=new boost::lockfree::queue<Packet*>(16);
			in_relayers[i]=new Relayer(in_endpoints[i],deviceProxy,hostProxy,in_queue[i]);

		}

		if (out_endpoints[i]) {
			//Relayer(Endpoint* _endpoint,DeviceProxy* _device,HostProxy* _host,boost::lockfree::queue<SetupPacket*>* _queue);
			if (i) {
				out_queue[i]=new boost::lockfree::queue<Packet*>(16);
				out_relayers[i]=new Relayer(out_endpoints[i],deviceProxy,hostProxy,out_queue[i]);
			} else {
				out_queue_ep0=new boost::lockfree::queue<SetupPacket*>(16);
				out_relayers[i]=new Relayer(this,out_endpoints[i],deviceProxy,hostProxy,out_queue_ep0);
			}
		}
	}


	//apply filters to relayers
	for(i=0;i<filterCount;i++) {
		if (filters[i]->test_device(device) && filters[i]->test_configuration(cfg)) {
			for (j=0;j<16;j++) {
				if (in_endpoints[j] && filters[i]->test_endpoint(in_endpoints[j]) && filters[i]->test_interface(in_endpoints[j]->get_interface())) {
					in_relayers[j]->add_filter(filters[i]);
				}
				if (out_endpoints[j] && filters[i]->test_endpoint(out_endpoints[j]) && filters[i]->test_interface(out_endpoints[j]->get_interface())) {
					out_relayers[j]->add_filter(filters[i]);
				}
			}
		}
	}

	//Claim interfaces
	for (ifc_idx=0;ifc_idx<ifc_cnt;ifc_idx++) {
		deviceProxy->claim_interface(ifc_idx);
	}

	//TODO set back to <16
	for(i=1;i<16;i++) {
		if (in_relayers[i]) {
			pthread_create(&in_relayerThreads[i],NULL,&Relayer::relay_helper,in_relayers[i]);
		}
		if (out_relayers[i]) {
			pthread_create(&out_relayerThreads[i],NULL,&Relayer::relay_helper,out_relayers[i]);
		}
	}

}

void Manager::stop_relaying(){
	if (status!=USBM_RELAYING && status!=USBM_SETUP) return;
	status=USBM_STOPPING;

	int i;
	//signal all injector threads to stop ASAP
	for(i=0;i<injectorCount;i++) {injectors[i]->halt=true;}

	//signal all relayer threads to stop ASAP
	for(i=0;i<16;i++) {
		if (in_relayers[i]) {in_relayers[i]->halt=true;}
		if (out_relayers[i]) {out_relayers[i]->halt=true;}
	}

	//wait for all injector threads to stop
	if (injectorThreads) {
		for(i=0;i<injectorCount;i++) {
			if (injectorThreads[i]) {
				pthread_join(injectorThreads[i],NULL);
				injectorThreads[i]=0;
			}
		}
		free(injectorThreads);
		injectorThreads=NULL;
	}

	//wait for all relayer threads to stop, then delete relayer objects
	for(i=0;i<16;i++) {
		if (in_endpoints[i]) {in_endpoints[i]=NULL;}
		if (in_relayers[i]) {
			if (in_relayerThreads[i]) {
				pthread_join(in_relayerThreads[i],NULL);
				in_relayerThreads[i]=0;
			}
			delete(in_relayers[i]);
			in_relayers[i]=NULL;
		}

		if (out_endpoints[i]) {
			//we created EP0 object, all others are under Device control
			if (!i) {delete(out_endpoints[i]);}
			out_endpoints[i]=NULL;
		}
		if (out_relayers[i]) {
			if (out_relayerThreads[i]) {
				pthread_join(out_relayerThreads[i],NULL);
				out_relayerThreads[i]=0;
			}
			delete(out_relayers[i]);
			out_relayers[i]=NULL;
		}
		if (in_queue[i]) {
			delete(in_queue[i]);
			in_queue[i]=NULL;
		}
		if (out_queue[i]) {
			delete(out_queue[i]);
			out_queue[i]=NULL;
		}
	}

	if (out_queue_ep0) {
		delete(out_queue_ep0);
		out_queue_ep0=NULL;
	}

	//Release interfaces
	int ifc_idx;
		if (device) {
		Configuration* cfg=device->get_active_configuration();
		int ifc_cnt=cfg->get_descriptor()->bNumInterfaces;
		for (ifc_idx=0;ifc_idx<ifc_cnt;ifc_idx++) {
			deviceProxy->release_interface(ifc_idx);
		}
	}

	//disconnect from host
	hostProxy->disconnect();

	//disconnect device proxy
	deviceProxy->disconnect();

	//clean up device model & endpoints
	if (device) {
		delete(device);
		device=NULL;
	}

	status=USBM_IDLE;
}

void Manager::setConfig(__u8 index) {
	device->set_active_configuration(index);
	DeviceQualifier* qualifier=device->get_device_qualifier();
	if (qualifier) {
		if (device->is_highspeed()) {
			deviceProxy->setConfig(device->get_device_qualifier()->get_configuration(index),device->get_configuration(index),true);
			hostProxy->setConfig(device->get_device_qualifier()->get_configuration(index),device->get_configuration(index),true);
		} else {
			deviceProxy->setConfig(device->get_configuration(index),device->get_device_qualifier()->get_configuration(index),false);
			hostProxy->setConfig(device->get_configuration(index),device->get_device_qualifier()->get_configuration(index),false);
		}
	} else {
		deviceProxy->setConfig(device->get_configuration(index),NULL,device->is_highspeed());
		hostProxy->setConfig(device->get_configuration(index),NULL,device->is_highspeed());
	}
	start_data_relaying();
}
