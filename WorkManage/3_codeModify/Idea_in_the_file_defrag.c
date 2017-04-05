static int file_defrag(const char *file, const struct stat64 *buf,
						int flag EXT2FS_ATTR((unused)),
						struct FTW *ftwbuf EXT2FS_ATTR((unused)))
 {   //struct FTW : http://ftp.stu.edu.tw/BSD/OpenBSD/src/include/ftw.h
     int fd;
     int donor_fd = -1;
     int ret;
     int best;
     int file_frags_start, file_frags_end;//defrag하기 전 frag의 개수, defrag 한 후의 frag개수
     int orig_physical_cnt, donor_physical_cnt = 0;
     char    tmp_inode_name[PATH_MAX + 8];
     ext4_fsblk_t            blk_count = 0;
     struct fiemap_extent_list   *orig_list_physical = NULL;//struct fiemap_extent_list는 line 134에 선언되어 있다.
     struct fiemap_extent_list   *orig_list_logical = NULL;
     struct fiemap_extent_list   *donor_list_physical = NULL;
     struct fiemap_extent_list   *donor_list_logical = NULL;
     struct fiemap_extent_group  *orig_group_head = NULL;
     struct fiemap_extent_group  *orig_group_tmp = NULL;
 
     defraged_file_count++;
 
	 //e4defrag 수행시 설정한 옵션에 '디프레그먼테이션 과정의 detail을 보여라'고 되있으면
	 //mode_flag & DETAIL 의 결과로 true가 나오도록 되어있나봄. 
     if (mode_flag & DETAIL) {
         printf("[%u/%u]", defraged_file_count, total_count);
         fflush(stdout);
     }
 
/////CheckBeforeGetFilePart START : defrag 대상파일이 이상하거나 defrag할 필요가 없음을 잡아냄
     if (lost_found_dir[0] != '\0' &&
         !memcmp(file, lost_found_dir, strnlen(lost_found_dir, PATH_MAX))) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             IN_FTW_PRINT_ERR_MSG(NGMSG_LOST_FOUND);
         }
         return 0;
     }
 
     if (!S_ISREG(buf->st_mode)) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             IN_FTW_PRINT_ERR_MSG(NGMSG_FILE_UNREG);
         }
         return 0;
     }
 
     /* Empty file */
     if (buf->st_size == 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             IN_FTW_PRINT_ERR_MSG("File size is 0");
         }
         return 0;
     }
 
     /* Has no blocks */
     if (buf->st_blocks == 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             STATISTIC_ERR_MSG("File has no blocks");
         }
         return 0;
     }
 
     fd = open64(file, O_RDWR);
     if (fd < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_OPEN);
         }
         return 0;
     }
/////CheckBeforeGetFilePart END

     /* Get file's extents : set 'orig_list_physical'
	  * 논문아이디어1: 애초에 get_file_extents()가 크기 k 미만의 file extent만 가져오도록 하는 것
	  * 고려할 것: file_defrag 수행 후 해당 파일의 모든 extent가 한 곳에 모여있다고 파일시스템이 인식했었다면 그것 또한 고쳐야 한다.
	  * 고려할 것2: file extents들의 change_physical_to_logical 이후가 다음과 같다고 해보자. 
			ext1(200 blks) - ext2(12 blks) - ext3(150 blks) - ext4(25 blks) - ext5(350 blks) - ext6(22 blks) - ext7(240 blks) - ext8(45 blks) 
					이 경우 아이디어를 적용한 defrag 후의 성능향상이 매우 미미할 수 있다.

	  */
     ret = get_file_extents(fd, &orig_list_physical); 
	 if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_EXTENT);
         }
         goto out;
     }
 
     /* Get the count of file's continuous physical region */
     orig_physical_cnt = get_physical_count(orig_list_physical);
 
     /* change_physical_to_logical: Change list from physical to logical 

	physical extents          -> got extents list sorted by logical addr
	
	(ex-jsj)
	 3(1)-2(1)-11(2)-5(2)-9(1) -> 2(1)-3(1)-5(2)-9(1)-11(2)] 
					     				ㄴ[blk2][blk3][blk5,blk6][blk9][blk11,12]
	#addr(count) : an extent having 'count' number of blocks and its firt (logical)blk address is 'addr'
	#[blk_n1,blk_n2,..,blk_nk] : an extent having k (logically) continuous blocks

	  */
     ret = change_physical_to_logical(&orig_list_physical,
                             &orig_list_logical);
     if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_EXTENT);
         }
         goto out;
     }
 
     /* get_logical_count: Count file fragments(=extents) before defrag 
	  * orig_physical_cnt 가 get_logical_count()의 값과 다른가?? - ㄴㄴㄴ
	  */
     file_frags_start = get_logical_count(orig_list_logical);
 
     blk_count = get_file_blocks(orig_list_logical);
     if (file_check(fd, buf, file, file_frags_start, blk_count) < 0)
         goto out;
 
     if (fsync(fd) < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO("Failed to sync(fsync)");
         }
         goto out;
     }
 
     if (current_uid == ROOT_UID)
         best = get_best_count(blk_count);
     else
         best = 1;
 
     if (file_frags_start <= best)
         goto check_improvement;
 
     /* join_extents: Combine extents to group

	grouping extents into extent_group. 
	if some extents have logically continuous block, they'll in the same group

	(ex-jsj)
	[blk2][blk3][blk5,blk6][blk9][blk11,12] 
		-> ext_group_list( eg1([blk2][blk3][blk5,blk6]) - eg2([blk9]) - eg3([blk11,12]) )		

      */
     ret = join_extents(orig_list_logical, &orig_group_head);//orig_group_head setting
     if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_EXTENT);
         }
         goto out;
     }
 
     /* Create donor inode */
     memset(tmp_inode_name, 0, PATH_MAX + 8);
     sprintf(tmp_inode_name, "%.*s.defrag",
                 (int)strnlen(file, PATH_MAX), file);
     donor_fd = open64(tmp_inode_name, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR);
     if (donor_fd < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             if (errno == EEXIST)
                 PRINT_ERR_MSG_WITH_ERRNO(
                 "File is being defraged by other program");
             else
                 PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_OPEN);
         }
         goto out;
     }
 
     /* Unlink donor inode */
     ret = unlink(tmp_inode_name);
     if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO("Failed to unlink");
         }
         goto out;
     }
 
     /* Allocate space for donor inode */
     orig_group_tmp = orig_group_head;
     do {
         ret = fallocate64(donor_fd, 0,
           (loff_t)orig_group_tmp->start->data.logical * block_size,
           (loff_t)orig_group_tmp->len * block_size);
         if (ret < 0) {
             if (mode_flag & DETAIL) {
                 PRINT_FILE_NAME(file);
                 PRINT_ERR_MSG_WITH_ERRNO("Failed to fallocate");
             }
             goto out;
         }
 
         orig_group_tmp = orig_group_tmp->next;
     } while (orig_group_tmp != orig_group_head);
 
     /* Get donor inode's extents */
     ret = get_file_extents(donor_fd, &donor_list_physical);
     if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_EXTENT);
         }
         goto out;
     }
 
     /* Calcuate donor inode's continuous physical region */
     donor_physical_cnt = get_physical_count(donor_list_physical);
 
     /* Change donor extent list from physical to logical */
     ret = change_physical_to_logical(&donor_list_physical,
                             &donor_list_logical);
     if (ret < 0) {
         if (mode_flag & DETAIL) {
             PRINT_FILE_NAME(file);
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_EXTENT);
         }
         goto out;
     }
 
 check_improvement:
     if (mode_flag & DETAIL) {
         if (file_frags_start != 1)
             frag_files_before_defrag++;
 
         extents_before_defrag += file_frags_start;
     }
 
     if (file_frags_start <= best ||
             orig_physical_cnt <= donor_physical_cnt) {
         printf("\033[79;0H\033[K[%u/%u]%s:\t%3d%%",
             defraged_file_count, total_count, file, 100);
         if (mode_flag & DETAIL)
             printf("  extents: %d -> %d",
                 file_frags_start, file_frags_start);
 
         printf("\t[ OK ]\n");
         succeed_cnt++;
 
         if (file_frags_start != 1)
             frag_files_after_defrag++;
 
         extents_after_defrag += file_frags_start;
         goto out;
     }
 
     /* Defrag the file */
     ret = call_defrag(fd, donor_fd, file, buf, donor_list_logical);
 
     /* Count file fragments after defrag and print extents info */
     if (mode_flag & DETAIL) {
         file_frags_end = file_frag_count(fd);
         if (file_frags_end < 0) {
             printf("\n");
             PRINT_ERR_MSG_WITH_ERRNO(NGMSG_FILE_INFO);
             goto out;
         }
 
         if (file_frags_end != 1)
             frag_files_after_defrag++;
 
         extents_after_defrag += file_frags_end;
 
         if (ret < 0)
             goto out;
 
         printf("  extents: %d -> %d",
             file_frags_start, file_frags_end);
         fflush(stdout);
     }
 
     if (ret < 0)
         goto out;
 
     printf("\t[ OK ]\n");
     fflush(stdout);
     succeed_cnt++;
 
 out:
     close(fd);
     if (donor_fd != -1)
         close(donor_fd);
     free_ext(orig_list_physical);
     free_ext(orig_list_logical);
     free_ext(donor_list_physical);
     free_exts_group(orig_group_head);
     return 0;
 }