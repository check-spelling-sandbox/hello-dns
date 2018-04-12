/* Goal: a fully standards compliant basic authoritative server. In <1000 lines.
   Non-goals: notifications, slaving zones, name compression, edns,
              performance
*/
#include <cstdint>
#include <vector>
#include <map>
#include <stdexcept>
#include "sclasses.hh"
#include "dns.hh"
#include "safearray.hh"
#include <thread>
#include <signal.h>
#include "record-types.hh"
#include "dns-storage.hh"

using namespace std;

void addAdditional(const DNSNode* bestzone, const DNSName& zone, const vector<DNSName>& toresolve, DNSMessageWriter& response)
{
  for(auto addname : toresolve ) {
    cout<<"Doing additional or glue lookup for "<<addname<<" in "<<zone<<endl;
    if(!addname.makeRelative(zone)) {
      cout<<addname<<" is not within our zone, not doing glue"<<endl;
      continue;
    }
    DNSName wuh;
    auto addnode = bestzone->find(addname, wuh);
    if(!addnode || !addname.empty())  {
      cout<<"  Found nothing, continuing"<<endl;
      continue;
    }
    for(auto& type : {DNSType::A, DNSType::AAAA}) {
      auto iter2 = addnode->rrsets.find(type);
      if(iter2 != addnode->rrsets.end()) {
        const auto& rrset = iter2->second;
        for(const auto& rr : rrset.contents) {
          response.putRR(DNSSection::Additional, wuh+zone, type, rrset.ttl, rr);
        }
      }
    }
  }  
}

bool processQuestion(const DNSNode& zones, DNSMessageReader& dm, const ComboAddress& local, const ComboAddress& remote, DNSMessageWriter& response)
{
  DNSName name;
  DNSType type;
  dm.getQuestion(name, type);
  DNSName origname=name; // we need this for error reporting, we munch the original name
  bool haveEDNS=false;
  cout<<"Received a query from "<<remote.toStringWithPort()<<" for "<<name<<" and type "<<type<<endl;
  uint16_t newsize=0;
  bool doBit=false;
  haveEDNS = dm.getEDNS(&newsize, &doBit);
  if(haveEDNS && newsize > sizeof(dnsheader))
    response.payload.resize(newsize - sizeof(dnsheader));

  try {
    response.dh = dm.dh;
    response.dh.ad = response.dh.ra = response.dh.aa = 0;
    response.dh.qr = 1;
    response.setQuestion(name, type);
    
    if(type == DNSType::AXFR || type == DNSType::IXFR)  {
      cout<<"Query was for AXFR or IXFR over UDP, can't do that"<<endl;
      response.dh.rcode = (int)RCode::Servfail;
      return true;
    }

    if(dm.dh.opcode != 0) {
      cout<<"Query had non-zero opcode "<<dm.dh.opcode<<", sending NOTIMP"<<endl;
      response.dh.rcode = (int)RCode::Notimp;
      return true;
    }
    
    DNSName zone;
    auto fnd = zones.find(name, zone);
    if(fnd && fnd->zone) {
      cout<<"---\nBest zone: "<<zone<<", name now "<<name<<", loaded: "<<(void*)fnd->zone<<endl;

      response.dh.aa = 1; 
    
      auto bestzone = fnd->zone;
      DNSName searchname(name), lastnode, zonecutname;
      const DNSNode* passedZonecut=0;
      int CNAMELoopCount = 0;
    
    loopCNAME:;
      auto node = bestzone->find(searchname, lastnode, &passedZonecut, &zonecutname);

      if(!node) {
        cout<<"Found nothing in zone '"<<zone<<"' for lhs '"<<name<<"'"<<endl;
      }
      else if(passedZonecut) {
        response.dh.aa = false;
        cout<<"This is a delegation, zonecutname: '"<<zonecutname<<"'"<<endl;
      
        for(const auto& rr: passedZonecut->rrsets) {
          cout<<"  Have type "<<rr.first<<endl;
        }
        auto iter = passedZonecut->rrsets.find(DNSType::NS);
        if(iter != passedZonecut->rrsets.end()) {
          const auto& rrset = iter->second;
          vector<DNSName> toresolve;
          for(const auto& rr : rrset.contents) {
            response.putRR(DNSSection::Authority, zonecutname+zone, DNSType::NS, rrset.ttl, rr);
            toresolve.push_back(dynamic_cast<NSGen*>(rr.get())->d_name);
          }
          addAdditional(bestzone, zone, toresolve, response);
        }
      }
      else if(!searchname.empty()) {
        cout<<"This is an NXDOMAIN situation"<<endl;
        const auto& rrset = fnd->zone->rrsets[DNSType::SOA];
        response.dh.rcode = (int)RCode::Nxdomain;
        response.putRR(DNSSection::Authority, zone, DNSType::SOA, rrset.ttl, rrset.contents[0]);
      }
      else {
        cout<<"Found something in zone '"<<zone<<"' for lhs '"<<name<<"', searchname now '"<<searchname<<"', lastnode '"<<lastnode<<"', passedZonecut="<<passedZonecut<<endl;
      
        auto iter = node->rrsets.cbegin();
        vector<DNSName> additional;
        if(iter = node->rrsets.find(DNSType::CNAME), iter != node->rrsets.end()) {
          cout<<"We have a CNAME!"<<endl;
          const auto& rrset = iter->second;
          DNSName target;
          for(const auto& rr : rrset.contents) {
            response.putRR(DNSSection::Answer, lastnode+zone, DNSType::CNAME, rrset.ttl, rr);
            target=dynamic_cast<CNAMEGen*>(rr.get())->d_name;
          }
          if(target.makeRelative(zone)) {
            cout<<"  Should follow CNAME to "<<target<<" within our zone"<<endl;
            // XXX we need to change our behaviour on NXDOMAIN I think depending on if you've followed a CNAME
            searchname = target; 
            if(CNAMELoopCount++ < 10) {
              lastnode.clear();
              zonecutname.clear();
              goto loopCNAME;
            }
          }
          else
            cout<<"  CNAME points to record "<<target<<" in other zone, good luck"<<endl;
        }
        else if(type == DNSType::ANY) {
          for(const auto& t : node->rrsets) {
            const auto& rrset = t.second;
            for(const auto& rr : rrset.contents) {
              response.putRR(DNSSection::Answer, lastnode+zone, t.first, rrset.ttl, rr);
              if(t.first == DNSType::MX)
                additional.push_back(dynamic_cast<MXGen*>(rr.get())->d_name);

            }
          }
        }
        else if(iter = node->rrsets.find(type), iter != node->rrsets.end() || type==DNSType::ANY) {
          const auto& rrset = iter->second;
          for(const auto& rr : rrset.contents) {
            response.putRR(DNSSection::Answer, lastnode+zone, type, rrset.ttl, rr);
            if(type == DNSType::MX)
              additional.push_back(dynamic_cast<MXGen*>(rr.get())->d_name);
          }
        }
        else {
          cout<<"Node exists, qtype doesn't, NOERROR situation, inserting SOA"<<endl;
          const auto& rrset = fnd->zone->rrsets[DNSType::SOA];
          response.putRR(DNSSection::Answer, zone, DNSType::SOA, rrset.ttl, rrset.contents[0]);
        }
        addAdditional(bestzone, zone, additional, response);
      }
    }
    else {
      cout<<"No zone matched"<<endl;
      response.dh.rcode = (uint8_t)RCode::Refused;
    }
    if(haveEDNS) {
      response.putEDNS(newsize, doBit);
    }
    return true;
  }
  catch(std::out_of_range& e) { // exceeded packet size
    cout<<"Query for '"<<origname<<"'|"<<type<<" got truncated"<<endl;
    response.setQuestion(origname, type); // this resets the packet
    response.dh.tc=1; response.dh.aa=0;
    if(haveEDNS) {
      response.putEDNS(newsize, doBit);
    }
      
    return true;
  }
  catch(std::exception& e) {
    cout<<"Error processing query: "<<e.what()<<endl;
    return false;
  }
}

void udpThread(ComboAddress local, Socket* sock, const DNSNode* zones)
{
  for(;;) {
    ComboAddress remote(local);
    DNSMessageReader dm;
    string message = SRecvfrom(*sock, sizeof(dm), remote);
    if(message.size() < sizeof(dnsheader)) {
      cerr<<"Dropping query from "<<remote.toStringWithPort()<<", too short"<<endl;
      continue;
    }
    memcpy(&dm, message.c_str(), message.size());

    if(dm.dh.qr) {
      cerr<<"Dropping non-query from "<<remote.toStringWithPort()<<endl;
      continue;
    }

    DNSMessageWriter response;
    if(processQuestion(*zones, dm, local, remote, response)) {
      cout<<"Sending response with rcode "<<(RCode)response.dh.rcode <<endl;
      SSendto(*sock, response.serialize(), remote);
    }
  }
}

void writeTCPResponse(int sock, const DNSMessageWriter& response)
{
  string ser="00"+response.serialize();
  cout<<"Sending a message of "<<ser.size()<<" bytes in response"<<endl;
  uint16_t len = htons(ser.length()-2);
  ser[0] = *((char*)&len);
  ser[1] = *(((char*)&len) + 1);
  SWriten(sock, ser);
}

void tcpClientThread(ComboAddress local, ComboAddress remote, int s, const DNSNode* zones)
{
  Socket sock(s);
  cout<<"TCP Connection from "<<remote.toStringWithPort()<<endl;
  for(;;) {
    uint16_t len;
    
    string message = SRead(sock, 2);
    if(message.size() != 2)
      break;
    memcpy(&len, &message.at(1)-1, 2);
    len=htons(len);
    
    if(len > 512) {
      cerr<<"Remote "<<remote.toStringWithPort()<<" sent question that was too big"<<endl;
      return;
    }
    
    if(len < sizeof(dnsheader)) {
      cerr<<"Dropping query from "<<remote.toStringWithPort()<<", too short"<<endl;
      return;
    }

    message = SRead(sock, len);
    DNSMessageReader dm;
    memcpy(&dm, message.c_str(), message.size());

    if(dm.dh.qr) {
      cerr<<"Dropping non-query from "<<remote.toStringWithPort()<<endl;
      return;
    }

    DNSName name;
    DNSType type;
    dm.getQuestion(name, type);
    DNSMessageWriter response(std::numeric_limits<uint16_t>::max()-sizeof(dnsheader));
    
    if(type == DNSType::AXFR) {
      cout<<"Should do AXFR for "<<name<<endl;

      DNSName zone;
      auto fnd = zones->find(name, zone);
      if(!fnd || !fnd->zone || !name.empty() || !fnd->zone->rrsets.count(DNSType::SOA)) {
        cout<<"   This was not a zone, or zone had no SOA"<<endl;
        return;
      }
      cout<<"Have zone, walking it"<<endl;
      response.dh = dm.dh;
      response.dh.ad = response.dh.ra = response.dh.aa = 0;
      response.dh.qr = 1;
      response.setQuestion(zone, type);

      auto node = fnd->zone;

      // send SOA
      response.putRR(DNSSection::Answer, zone, DNSType::SOA, node->rrsets[DNSType::SOA].ttl, node->rrsets[DNSType::SOA].contents[0]);

      writeTCPResponse(sock, response);
      response.setQuestion(zone, type);

      // send all other records
      node->visit([&response,&sock,&name,&type,&zone](const DNSName& nname, const DNSNode* n) {
          for(const auto& p : n->rrsets) {
            if(p.first == DNSType::SOA)
              continue;
            for(const auto& rr : p.second.contents) {
            retry:
              try {
                response.putRR(DNSSection::Answer, nname, p.first, p.second.ttl, rr);
              }
              catch(std::out_of_range& e) { // exceeded packet size 
                writeTCPResponse(sock, response);
                response.setQuestion(zone, type);
                goto retry;
              }
            }
          }
        }, zone);

      writeTCPResponse(sock, response);
      response.setQuestion(zone, type);

      // send SOA again
      response.putRR(DNSSection::Answer, zone, DNSType::SOA, node->rrsets[DNSType::SOA].ttl, node->rrsets[DNSType::SOA].contents[0]);

      writeTCPResponse(sock, response);
      return;
    }
    else {
      dm.payload.rewind();
      
      if(processQuestion(*zones, dm, local, remote, response)) {
        writeTCPResponse(sock, response);
      }
      else
        return;
    }
  }
}

int main(int argc, char** argv)
try
{
  if(argc != 2) {
    cerr<<"Syntax: tdns ipaddress:port"<<endl;
    return(EXIT_FAILURE);
  }
  signal(SIGPIPE, SIG_IGN);
  ComboAddress local(argv[1], 53);

  Socket udplistener(local.sin4.sin_family, SOCK_DGRAM);
  SBind(udplistener, local);

  Socket tcplistener(local.sin4.sin_family, SOCK_STREAM);
  SSetsockopt(tcplistener, SOL_SOCKET, SO_REUSEPORT, 1);
  SBind(tcplistener, local);
  SListen(tcplistener, 10);
  
  DNSNode zones;
  loadZones(zones);
  
  thread udpServer(udpThread, local, &udplistener, &zones);

  for(;;) {
    ComboAddress remote;
    int client = SAccept(tcplistener, remote);
    thread t(tcpClientThread, local, remote, client, &zones);
    t.detach();
  }
}
catch(std::exception& e)
{
  cerr<<"Fatal error: "<<e.what()<<endl;
  return EXIT_FAILURE;
}
