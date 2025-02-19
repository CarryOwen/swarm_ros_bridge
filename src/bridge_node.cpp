/**
 * @file bridge_node.cpp
 * @author Peixuan Shu (shupeixuan@qq.com)
 * @brief Reliable TCP bridge for ros data transfer in unstable network.
 * It will send/receive the specified ROS topics in ../config/ros_topics.yaml
 * It uses zmq socket(PUB/SUB mode), which reconnects others autonomously and
 * supports 1-N pub-sub connection even with TCP protocol.
 *
 * Note: This program relies on ZMQPP (c++ wrapper around ZeroMQ).
 *  sudo apt install libzmqpp-dev
 *
 * Core Idea: It would create the receving thread for each receiving ROS topic
 *  and send ROS messages in each sub_cb() callback.
 *
 * @version 1.0
 * @date 2023-01-01
 *
 * @license BSD 3-Clause License
 * @copyright (c) 2023, Peixuan Shu
 * All rights reserved.
 *
 */

#include "bridge_node.hpp"
/* uniform callback functions for ROS subscribers */
template <typename T, int i>
void sub_cb(const T &msg)
{
  /* frequency control */
  std::cout << "Got other ROS topic!" << msg << std::endl;
  ros::Time t_now = ros::Time::now();
  if ((t_now - sub_t_last[i]).toSec() * sendTopics[i].max_freq < 1.0)
  {
    return;
  }
  sub_t_last[i] = t_now;

  /* serialize the sending messages into send_buffer */
  namespace ser = ros::serialization;
  size_t data_len = ser::serializationLength(msg);             // bytes length of msg
  std::cout << "msg lengh:" << data_len << std::endl;
  std::unique_ptr<uint8_t> send_buffer(new uint8_t[data_len]); // create a dynamic length array
  ser::OStream stream(send_buffer.get(), data_len);
  ser::serialize(stream, msg);
  /* zmq send message */
  // zmqpp::message send_array;
  // /* equal to:
  //   send_array.add_raw(reinterpret_cast<void const*>(&data_len), sizeof(size_t));
  // */
   zmqpp::message send_array;
  send_array << data_len;
  /* equal to:
    send_array.add_raw(reinterpret_cast<void const*>(&data_len), sizeof(size_t));
  */
  send_array.add_raw(reinterpret_cast<void const *>(send_buffer.get()), data_len);

  //get(1)可以获取到数据
  std::cout << "msg::" << send_array.get(1) << std::endl;

  std::cout << "ready send!" << std::endl;
  // // send(&, true) for non-blocking, send(&, false) for blocking
  // bool dont_block = false; // Actually for PUB mode zmq socket, send() will never block
  zmq::message_t msg_t;
  msg_t.rebuild(send_array.get(1).data(), send_array.get(1).size());
  senders[i]->send(msg_t, zmq::send_flags::dontwait);
  std::cout << "send!" << std::endl;

  // std::cout << msg << std::endl;
  // std::cout << i << std::endl;
}

/* uniform deserialize and publish the receiving messages */
template <typename T>
void deserialize_pub(uint8_t *buffer_ptr, size_t msg_size, int i)
{
  T msg;
  // deserialize the receiving messages into ROS msg
  namespace ser = ros::serialization;
  ser::IStream stream(buffer_ptr, msg_size);
  ser::deserialize(stream, msg);
  // publish ROS msg
  topic_pubs[i].publish(msg);
}

/* receive thread function to receive messages through ZMQ  and publish them through ROS */
void recv_func(int i)
{
  while (recv_thread_flags[i])
  {
    // /* receive and process message */
    // zmqpp::message recv_array;
    // bool recv_flag; // receive success flag
    // // std::cout << "ready receive!" << std::endl;
    // // receive(&,true) for non-blocking, receive(&,false) for blocking
    // bool dont_block = false; // 'true' leads to high cpu load
    // if (recv_flag = receivers[i]->receive(recv_array, dont_block))
    // {
    //   std::cout << "receive:" << recv_array.get(0) << std::endl;
    //   size_t data_len;
    //   recv_array >> data_len; // unpack meta data
    //   // /*  equal to:
    //   //   recv_array.get(&data_len, recv_array.read_cursor++);
    //   //   void get(T &value, size_t const cursor){
    //   //     uint8_t const* byte = static_cast<uint8_t const*>(raw_data(cursor));
    //   //     b = *byte;}
    //   // */
    //   // // a dynamic length array by unique_ptr
    //   // std::unique_ptr<uint8_t> recv_buffer(new uint8_t[data_len]);
    //   // // continue to copy the raw_data of recv_array into buffer
    //   // memcpy(recv_buffer.get(), static_cast<const uint8_t *>(recv_array.raw_data(recv_array.read_cursor())), data_len);
    //   // deserialize_publish(recv_buffer.get(), data_len, recvTopics[i].type, i);

    //   // std::cout << data_len << std::endl;
    //   // std::cout << recv_buffer.get() << std::endl;
    // }

    // /* if receive() does not block, sleep to decrease loop rate */
    // if (dont_block)
    //   std::this_thread::sleep_for(std::chrono::microseconds(1000)); // sleep for us
    // else
    // {
    //   /* check and report receive state */
    //   if (recv_flag != recv_flags_last[i]) // false -> true(first message in)
    //     ROS_INFO("[bridge node] \"%s\" received!", recvTopics[i].name.c_str());
    //   recv_flags_last[i] = recv_flag;
    // }
  }
  return;
}

/* close recv socket, unsubscribe ROS topic */
void stop_send(int i)
{
  // senders[i]->unbind(std::string const &endpoint);
  senders[i]->close();      // close the send socket
  topic_subs[i].shutdown(); // unsubscribe
}

/* stop recv thread, close recv socket, unadvertise ROS topic */
void stop_recv(int i)
{
  recv_thread_flags[i] = false; // finish recv_func()
  // receivers[i]->disconnect(std::string &endpoint);
  receivers[i]->close();    // close the receive socket
  topic_pubs[i].shutdown(); // unadvertise
}

// TODO: generate or delete topic message transfers through a remote zmq service.

int main(int argc, char **argv)
{
  ros::init(argc, argv, "swarm_bridge");
  ros::NodeHandle nh("~");

  //************************ Parse configuration file **************************
  // get hostnames and IPs
  // 从yaml文件中获取IP,yaml文件是由launch中文件指定的
  if (nh.getParam("IP", ip_xml) == false)
  {
    ROS_ERROR("[bridge node] No IP found in the configuration!");
    return 1;
  }
  // get "send topics" params (topic_name, topic_type, IP, port)
  // 获取主题信息，保存在send_topics_xml中，
  if (nh.getParam("send_topics", send_topics_xml))
  {
    ROS_ASSERT(send_topics_xml.getType() == XmlRpc::XmlRpcValue::TypeArray);
    len_send = send_topics_xml.size();
  }
  else
  {
    ROS_WARN("[bridge node] No send_topics found in the configuration!");
    len_send = 0;
  }
  // get "receive topics" params (topic_name, topic_type, IP, port)
  // 获取主题，存进XmlRpcValue类型的数据里面
  if (nh.getParam("recv_topics", recv_topics_xml))
  {
    // 断言数据类型
    ROS_ASSERT(recv_topics_xml.getType() == XmlRpc::XmlRpcValue::TypeArray);
    len_recv = recv_topics_xml.size();
  }
  else
  {
    ROS_WARN("[bridge node] No recv_topics found in the configuration!");
    len_recv = 0;
  }

  if (len_send > SUB_MAX)
  {
    ROS_FATAL("[bridge_node] The number of send topics in configuration exceeds the limit %d!", SUB_MAX);
    return 2;
  }
  // xmlrpc++ 支持类似stl库内的迭代器，遍历上面读到的数据
  std::cout << "-------------IP------------" << std::endl;
  for (auto iter = ip_xml.begin(); iter != ip_xml.end(); ++iter)
  {
    std::string host_name = iter->first;
    std::string host_ip = iter->second;
    std::cout << host_name << " : " << host_ip << std::endl;
    // 在ip_map里面存储所有的hostname和ip，并检查是否有重复的name
    if (ip_map.find(host_name) != ip_map.end())
    { // ip_xml will never contain same names actually.
      ROS_WARN("[bridge node] IPs with the same name in configuration %s!", host_name.c_str());
    }
    ip_map[host_name] = host_ip;
  }

  std::cout << "--------send topics--------" << std::endl;
  std::set<int> srcPorts; // for duplicate check
  for (int32_t i = 0; i < len_send; ++i)
  {
    ROS_ASSERT(send_topics_xml[i].getType() == XmlRpc::XmlRpcValue::TypeStruct);
    XmlRpc::XmlRpcValue send_topic_xml = send_topics_xml[i];
    std::string topic_name = send_topic_xml["topic_name"];
    std::string msg_type = send_topic_xml["msg_type"];
    int max_freq = send_topic_xml["max_freq"];
    std::string srcIP = ip_map[send_topic_xml["srcIP"]];
    int srcPort = send_topic_xml["srcPort"];
    TopicInfo topic = {.name = topic_name, .type = msg_type, .max_freq = max_freq, .ip = srcIP, .port = srcPort};
    // 将所有的topic信息放进vector里面
    sendTopics.emplace_back(topic);
    // check for duplicate ports:
    if (srcPorts.find(srcPort) != srcPorts.end())
    {
      ROS_FATAL("[bridge_node] Send topics with the same srcPort %d in configuration!", srcPort);
      return 3;
    }
    srcPorts.insert(srcPort); // for duplicate check
    std::cout << topic.name << "  " << topic.max_freq << "Hz(max)" << std::endl;
  }

  std::cout << "-------receive topics------" << std::endl;
  for (int32_t i = 0; i < len_recv; ++i)
  {
    ROS_ASSERT(recv_topics_xml[i].getType() == XmlRpc::XmlRpcValue::TypeStruct);
    XmlRpc::XmlRpcValue recv_topic_xml = recv_topics_xml[i];
    std::string topic_name = recv_topic_xml["topic_name"];
    std::string msg_type = recv_topic_xml["msg_type"];
    int max_freq = recv_topic_xml["max_freq"];
    std::string srcIP = ip_map[recv_topic_xml["srcIP"]];
    int srcPort = recv_topic_xml["srcPort"];
    TopicInfo topic = {.name = topic_name, .type = msg_type, .max_freq = max_freq, .ip = srcIP, .port = srcPort};
    recvTopics.emplace_back(topic);
    std::cout << topic.name << std::endl;
  }

  // ********************* zmq socket initialize ***************************
  // send sockets (zmq socket PUB mode)
  // 根据topic的个数创建zmq上下文，并绑定到设置的地址和端口，这里创建的是server发布端，用来发布消息给外部进程
  std::cout << "-------send topics' info------:" << std ::endl;
  for (int32_t i = 0; i < len_send; ++i)
  {
    std::shared_ptr<zmq2wrapper::Zmq2wpPub> server_pub;
    const std::string url = "tcp://" + sendTopics[i].ip + ":" + std::to_string(sendTopics[i].port);
    std::cout << "url: " << url << "  topic: " << sendTopics[i].name << "port: " << sendTopics[i].port << std ::endl;
    std::unique_ptr<zmq::socket_t> sender(new zmq::socket_t(context, zmq::socket_type::pub));
    sender->bind(url);
    senders.emplace_back(std::move(sender)); // sender is now released by std::move
  }

  // receive sockets (zmq socket SUB mode)
  // 根据topic的数量，创建client订阅端,用来订阅外部进程的消息
  std::cout << "-------receive topics' info------" << std ::endl;
  for (int32_t i = 0; i < len_recv; ++i)
  {
    const std::string url = "tcp://" + recvTopics[i].ip + ":" + std::to_string(recvTopics[i].port);
    std::cout << "url: " << url << "  topic: " << recvTopics[i].name << std::endl;
    std::string const zmq_topic = ""; // "" means all zmq topic
    std::unique_ptr<zmq::socket_t> receiver(new zmq::socket_t(context, zmq::socket_type::sub));
    // receiver->subscribe(recvTopics[i].name.c_str()); // 这个就是类似zmq_setsockopt设置过滤条件
    receiver->setsockopt(ZMQ_SUBSCRIBE,zmq_topic.c_str(),zmq_topic.length());
    receiver->connect(url);
    receivers.emplace_back(std::move(receiver));
  }

  // ******************* ROS subscribe and publish *************************
  // ROS topic subsrcibe and send
  // 这里发布的topic是指当前ros系统自己发布的，因此需要先订阅，然后通过zmq再转发给其他的ros或者进程
  for (int32_t i = 0; i < len_send; ++i)
  {
    sub_t_last.emplace_back(ros::Time::now()); // sub_cb called last time
    ros::Subscriber subscriber;
    // The uniform callback function is sub_cb()
    std::cout << "Ros subscribe topic:" << sendTopics[i].name << std::endl;
    subscriber = topic_subscriber(sendTopics[i].name, sendTopics[i].type, nh, i);
    topic_subs.emplace_back(subscriber);
    // use topic_subs[i].shutdown() to unsubscribe
  }

  // ROS topic receive and publish
  // 这里的 接收是指通过zmq接收其他ros或者进程的消息，然后publish出去
  for (int32_t i = 0; i < len_recv; ++i)
  {
    ros::Publisher publisher;
    publisher = topic_publisher(recvTopics[i].name, recvTopics[i].type, nh);
    topic_pubs.emplace_back(publisher);
  }

  // ****************** launch receive threads *****************************
  for (int32_t i = 0; i < len_recv; ++i)
  {
    recv_thread_flags.emplace_back(true);                  // enable receive thread flags
    recv_flags_last.emplace_back(false);                   // receive success flag
    recv_threads.emplace_back(std::thread(&recv_func, i)); // 启动线程，这里不调用join或者detch的原因是spin也会等待，所以不需要再阻塞主线程了
  }

  ros::spin();

  // ***************** stop send/receive ******************************
  for (int32_t i = 0; i < len_send; ++i)
  {
    stop_send(i);
  }

  for (int32_t i = 0; i < len_recv; ++i)
  {
    stop_recv(i);
  }

  return 0;
}