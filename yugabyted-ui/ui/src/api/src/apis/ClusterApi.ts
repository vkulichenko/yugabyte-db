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

// eslint-disable-next-line @typescript-eslint/ban-ts-comment
// @ts-ignore
import { useQuery, useInfiniteQuery, useMutation, UseQueryOptions, UseInfiniteQueryOptions, UseMutationOptions } from 'react-query';
import Axios from '../runtime';
import type { AxiosInstance } from 'axios';
// eslint-disable-next-line @typescript-eslint/ban-ts-comment
// @ts-ignore
import type {
  ApiError,
  ClusterListResponse,
  ClusterResponse,
  ClusterSpec,
  CreateClusterRequest,
  NodeOpRequest,
  NodeOperationResponse,
} from '../models';

export interface CreateClusterForQuery {
  accountId: string;
  projectId: string;
  CreateClusterRequest?: CreateClusterRequest;
}
export interface EditClusterForQuery {
  ClusterSpec?: ClusterSpec;
}
export interface ListClustersForQuery {
  accountId: string;
  projectId: string;
  name?: string;
  cloud?: string;
  region?: string;
  state?: string;
  tier?: ListClustersTierEnum;
  vpc_id?: string;
  scheduled_upgrade_execution_id?: string;
  order?: string;
  order_by?: string;
  limit?: number;
  continuation_token?: string;
}
export interface NodeOpForQuery {
  accountId: string;
  projectId: string;
  clusterId: string;
  NodeOpRequest?: NodeOpRequest;
}
export interface PauseClusterForQuery {
  accountId: string;
  projectId: string;
  clusterId: string;
}
export interface ResumeClusterForQuery {
  accountId: string;
  projectId: string;
  clusterId: string;
}

/**
 * Create a new Yugabyte Cluster
 * Create a cluster
 */


export const createClusterMutate = (
  body: CreateClusterForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/public/accounts/{accountId}/projects/{projectId}/clusters'.replace(`{${'accountId'}}`, encodeURIComponent(String(body.accountId))).replace(`{${'projectId'}}`, encodeURIComponent(String(body.projectId)));
  // eslint-disable-next-line
  // @ts-ignore
  delete body.accountId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.projectId;
  return Axios<ClusterResponse>(
    {
      url,
      method: 'POST',
      data: body.CreateClusterRequest
    },
    customAxiosInstance
  );
};

export const useCreateClusterMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<ClusterResponse, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<ClusterResponse, Error, CreateClusterForQuery, unknown>((props) => {
    return  createClusterMutate(props, customAxiosInstance);
  }, mutationOptions);
};


/**
 * Submit task to delete a Yugabyte Cluster
 * Submit task to delete a cluster
 */


export const deleteClusterMutate = (
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/cluster';
  return Axios<unknown>(
    {
      url,
      method: 'DELETE',
    },
    customAxiosInstance
  );
};

export const useDeleteClusterMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<unknown, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<unknown, Error, void, unknown>(() => {
    return  deleteClusterMutate(customAxiosInstance);
  }, mutationOptions);
};


/**
 * Submit task to edit a Yugabyte Cluster
 * Submit task to edit a cluster
 */


export const editClusterMutate = (
  body: EditClusterForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/cluster';
  return Axios<ClusterResponse>(
    {
      url,
      method: 'PUT',
      data: body.ClusterSpec
    },
    customAxiosInstance
  );
};

export const useEditClusterMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<ClusterResponse, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<ClusterResponse, Error, EditClusterForQuery, unknown>((props) => {
    return  editClusterMutate(props, customAxiosInstance);
  }, mutationOptions);
};


/**
 * Get a Yugabyte Cluster
 * Get a cluster
 */

export const getClusterAxiosRequest = (
  customAxiosInstance?: AxiosInstance
) => {
  return Axios<ClusterResponse>(
    {
      url: '/cluster',
      method: 'GET',
      params: {
      }
    },
    customAxiosInstance
  );
};

export const getClusterQueryKey = (
  pageParam = -1,
  version = 1,
) => [
  `/v${version}/cluster`,
  pageParam,
];


export const useGetClusterInfiniteQuery = <T = ClusterResponse, Error = ApiError>(
  options?: {
    query?: UseInfiniteQueryOptions<ClusterResponse, Error, T>;
    customAxiosInstance?: AxiosInstance;
  },
  pageParam = -1,
  version = 1,
) => {
  const queryKey = getClusterQueryKey(pageParam, version);
  const { query: queryOptions, customAxiosInstance } = options ?? {};

  const query = useInfiniteQuery<ClusterResponse, Error, T>(
    queryKey,
    () => getClusterAxiosRequest(customAxiosInstance),
    queryOptions
  );

  return {
    queryKey,
    ...query
  };
};

export const useGetClusterQuery = <T = ClusterResponse, Error = ApiError>(
  options?: {
    query?: UseQueryOptions<ClusterResponse, Error, T>;
    customAxiosInstance?: AxiosInstance;
  },
  version = 1,
) => {
  const queryKey = getClusterQueryKey(version);
  const { query: queryOptions, customAxiosInstance } = options ?? {};

  const query = useQuery<ClusterResponse, Error, T>(
    queryKey,
    () => getClusterAxiosRequest(customAxiosInstance),
    queryOptions
  );

  return {
    queryKey,
    ...query
  };
};



/**
 * List clusters
 * List clusters
 */

export const listClustersAxiosRequest = (
  requestParameters: ListClustersForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  return Axios<ClusterListResponse>(
    {
      url: '/public/accounts/{accountId}/projects/{projectId}/clusters'.replace(`{${'accountId'}}`, encodeURIComponent(String(requestParameters.accountId))).replace(`{${'projectId'}}`, encodeURIComponent(String(requestParameters.projectId))),
      method: 'GET',
      params: {
        name: requestParameters['name'],
        cloud: requestParameters['cloud'],
        region: requestParameters['region'],
        state: requestParameters['state'],
        tier: requestParameters['tier'],
        vpc_id: requestParameters['vpc_id'],
        scheduled_upgrade_execution_id: requestParameters['scheduled_upgrade_execution_id'],
        order: requestParameters['order'],
        order_by: requestParameters['order_by'],
        limit: requestParameters['limit'],
        continuation_token: requestParameters['continuation_token'],
      }
    },
    customAxiosInstance
  );
};

export const listClustersQueryKey = (
  requestParametersQuery: ListClustersForQuery,
  pageParam = -1,
  version = 1,
) => [
  `/v${version}/public/accounts/{accountId}/projects/{projectId}/clusters`,
  pageParam,
  ...(requestParametersQuery ? [requestParametersQuery] : [])
];


export const useListClustersInfiniteQuery = <T = ClusterListResponse, Error = ApiError>(
  params: ListClustersForQuery,
  options?: {
    query?: UseInfiniteQueryOptions<ClusterListResponse, Error, T>;
    customAxiosInstance?: AxiosInstance;
  },
  pageParam = -1,
  version = 1,
) => {
  const queryKey = listClustersQueryKey(params, pageParam, version);
  const { query: queryOptions, customAxiosInstance } = options ?? {};

  const query = useInfiniteQuery<ClusterListResponse, Error, T>(
    queryKey,
    () => listClustersAxiosRequest(params, customAxiosInstance),
    queryOptions
  );

  return {
    queryKey,
    ...query
  };
};

export const useListClustersQuery = <T = ClusterListResponse, Error = ApiError>(
  params: ListClustersForQuery,
  options?: {
    query?: UseQueryOptions<ClusterListResponse, Error, T>;
    customAxiosInstance?: AxiosInstance;
  },
  version = 1,
) => {
  const queryKey = listClustersQueryKey(params,  version);
  const { query: queryOptions, customAxiosInstance } = options ?? {};

  const query = useQuery<ClusterListResponse, Error, T>(
    queryKey,
    () => listClustersAxiosRequest(params, customAxiosInstance),
    queryOptions
  );

  return {
    queryKey,
    ...query
  };
};



/**
 * Submit task to operate on a node
 * Submit task to operate on a node
 */


export const nodeOpMutate = (
  body: NodeOpForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/public/accounts/{accountId}/projects/{projectId}/clusters/{clusterId}/nodes/op'.replace(`{${'accountId'}}`, encodeURIComponent(String(body.accountId))).replace(`{${'projectId'}}`, encodeURIComponent(String(body.projectId))).replace(`{${'clusterId'}}`, encodeURIComponent(String(body.clusterId)));
  // eslint-disable-next-line
  // @ts-ignore
  delete body.accountId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.projectId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.clusterId;
  return Axios<NodeOperationResponse>(
    {
      url,
      method: 'POST',
      data: body.NodeOpRequest
    },
    customAxiosInstance
  );
};

export const useNodeOpMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<NodeOperationResponse, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<NodeOperationResponse, Error, NodeOpForQuery, unknown>((props) => {
    return  nodeOpMutate(props, customAxiosInstance);
  }, mutationOptions);
};


/**
 * Submit task to pause a Yugabyte Cluster
 * Submit task to pause a cluster
 */


export const pauseClusterMutate = (
  body: PauseClusterForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/public/accounts/{accountId}/projects/{projectId}/clusters/{clusterId}/pause'.replace(`{${'accountId'}}`, encodeURIComponent(String(body.accountId))).replace(`{${'projectId'}}`, encodeURIComponent(String(body.projectId))).replace(`{${'clusterId'}}`, encodeURIComponent(String(body.clusterId)));
  // eslint-disable-next-line
  // @ts-ignore
  delete body.accountId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.projectId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.clusterId;
  return Axios<ClusterResponse>(
    {
      url,
      method: 'POST',
    },
    customAxiosInstance
  );
};

export const usePauseClusterMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<ClusterResponse, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<ClusterResponse, Error, PauseClusterForQuery, unknown>((props) => {
    return  pauseClusterMutate(props, customAxiosInstance);
  }, mutationOptions);
};


/**
 * Submit task to resume a Yugabyte Cluster
 * Submit task to resume a cluster
 */


export const resumeClusterMutate = (
  body: ResumeClusterForQuery,
  customAxiosInstance?: AxiosInstance
) => {
  const url = '/public/accounts/{accountId}/projects/{projectId}/clusters/{clusterId}/resume'.replace(`{${'accountId'}}`, encodeURIComponent(String(body.accountId))).replace(`{${'projectId'}}`, encodeURIComponent(String(body.projectId))).replace(`{${'clusterId'}}`, encodeURIComponent(String(body.clusterId)));
  // eslint-disable-next-line
  // @ts-ignore
  delete body.accountId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.projectId;
  // eslint-disable-next-line
  // @ts-ignore
  delete body.clusterId;
  return Axios<ClusterResponse>(
    {
      url,
      method: 'POST',
    },
    customAxiosInstance
  );
};

export const useResumeClusterMutation = <Error = ApiError>(
  options?: {
    mutation?:UseMutationOptions<ClusterResponse, Error>,
    customAxiosInstance?: AxiosInstance;
  }
) => {
  const {mutation: mutationOptions, customAxiosInstance} = options ?? {};
  // eslint-disable-next-line
  // @ts-ignore
  return useMutation<ClusterResponse, Error, ResumeClusterForQuery, unknown>((props) => {
    return  resumeClusterMutate(props, customAxiosInstance);
  }, mutationOptions);
};






/**
  * @export
  * @enum {string}
  */
export enum ListClustersTierEnum {
  Free = 'FREE',
  Paid = 'PAID'
}
