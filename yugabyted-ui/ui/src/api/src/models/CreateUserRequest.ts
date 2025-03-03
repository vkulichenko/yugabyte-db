// tslint:disable
/**
 * Yugabyte Cloud
 * YugabyteDB as a Service
 *
 * The version of the OpenAPI document: v1
 * Contact: support@yugabyte.com
 *
 * NOTE: This class is auto generated by OpenAPI Generator (https://openapi-generator.tech).
 * https://openapi-generator.tech
 * Do not edit the class manually.
 */


// eslint-disable-next-line no-duplicate-imports
import type { UserSpec } from './UserSpec';


/**
 * Create user request
 * @export
 * @interface CreateUserRequest
 */
export interface CreateUserRequest  {
  /**
   * 
   * @type {UserSpec}
   * @memberof CreateUserRequest
   */
  user_spec: UserSpec;
  /**
   * Password associated with the user
   * @type {string}
   * @memberof CreateUserRequest
   */
  password: string;
  /**
   * Token from hCaptcha service
   * @type {string}
   * @memberof CreateUserRequest
   */
  hcaptcha_token?: string;
}



