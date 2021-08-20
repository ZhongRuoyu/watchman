/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.facebook.watchman;

import java.util.Map;

public class WatchmanException extends Exception {

  private final Map<String, Object> response;

  public WatchmanException() {
    super();
    response = null;
  }

  public WatchmanException(String reason) {
    super(reason);
    response = null;
  }

  public WatchmanException(String error, Map<String, Object> response) {
    super(error);
    this.response = response;
  }

  public Map<String, Object> getResponse() {
    return response;
  }
}
