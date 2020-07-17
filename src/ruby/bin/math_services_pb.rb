# Generated by the protocol buffer compiler.  DO NOT EDIT!
# Source: math.proto for package 'math'
# Original file comments:
# Copyright 2015 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

require 'grpc'
require 'math_pb'

module Math
  module Math
    class Service

      include GRPC::GenericService

      self.marshal_class_method = :encode
      self.unmarshal_class_method = :decode
      self.service_name = 'math.Math'

      # Div divides DivArgs.dividend by DivArgs.divisor and returns the quotient
      # and remainder.
      rpc :Div, Math::DivArgs, Math::DivReply
      # DivMany accepts an arbitrary number of division args from the client stream
      # and sends back the results in the reply stream.  The stream continues until
      # the client closes its end; the server does the same after sending all the
      # replies.  The stream ends immediately if either end aborts.
      rpc :DivMany, stream(Math::DivArgs), stream(Math::DivReply)
      # Fib generates numbers in the Fibonacci sequence.  If FibArgs.limit > 0, Fib
      # generates up to limit numbers; otherwise it continues until the call is
      # canceled.  Unlike Fib above, Fib has no final FibReply.
      rpc :Fib, Math::FibArgs, stream(Math::Num)
      # Sum sums a stream of numbers, returning the final result once the stream
      # is closed.
      rpc :Sum, stream(Math::Num), Math::Num
    end

    Stub = Service.rpc_stub_class
  end
end
